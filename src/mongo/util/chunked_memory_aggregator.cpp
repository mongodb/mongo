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

#include "mongo/util/chunked_memory_aggregator.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/concurrent_memory_aggregator.h"

namespace mongo {

ChunkedMemoryAggregator::ChunkedMemoryAggregator(ChunkedMemoryAggregator::PrivateTag,
                                                 ChunkedMemoryAggregator::Options options,
                                                 int64_t id,
                                                 ConcurrentMemoryAggregator* parent)
    : _options(std::move(options)), _id(id), _parent(parent) {}

ChunkedMemoryAggregator::~ChunkedMemoryAggregator() {
    // Ensure that all `MemoryUsageHandle` instances have gone out of scope
    // and freed their memory before the `ChunkedMemoryAggregator` goes out of scope.
    dassert(_curMemoryUsageBytes.load() == 0);
}

MemoryUsageHandle ChunkedMemoryAggregator::createUsageHandle(int64_t initialBytes) {
    return MemoryUsageHandle(initialBytes, this);
}

int64_t ChunkedMemoryAggregator::getCurrentMemoryUsageBytes() const {
    return _curMemoryUsageBytes.load();
}

int64_t ChunkedMemoryAggregator::getId() const {
    return _id;
}

void ChunkedMemoryAggregator::poll() {
    _parent->poll(this);
}

void ChunkedMemoryAggregator::add(int64_t delta) {
    int64_t oldValue = _curMemoryUsageBytes.fetchAndAdd(delta);
    int64_t newValue = oldValue + delta;
    int64_t update = computeUpstreamUpdate(oldValue, newValue);
    dassert(newValue >= 0);

    // Only send an update if we've accumulated a significant amount of memory
    // that's worth notifying about.
    if (update != 0) {
        _parent->add(this, update);
    }
}

int64_t ChunkedMemoryAggregator::computeUpstreamUpdate(int64_t oldValue, int64_t newValue) const {
    // Computes the delta update that should be propagated to the upstream memory aggregator. This
    // returns zero if no update should be sent.
    //
    // This is computed by rounding the old memory usage and the new memory usage values up to the
    // next `memoryUsageUpdateBatchSize` multiple. If the old and new memory usage values round up
    // to different boundaries, then that difference will be sent as the update to the upstream
    // memory aggregator, which will always be a multiple of `memoryUsageUpdateBatchSize` since this
    // only sends updates in chunks/batches.
    int64_t newValueBoundary = (newValue + (_options.memoryUsageUpdateBatchSize - 1)) /
        _options.memoryUsageUpdateBatchSize;
    int64_t oldValueBoundary = (oldValue + (_options.memoryUsageUpdateBatchSize - 1)) /
        _options.memoryUsageUpdateBatchSize;
    return (newValueBoundary - oldValueBoundary) * _options.memoryUsageUpdateBatchSize;
}

};  // namespace mongo

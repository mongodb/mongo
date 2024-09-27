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

#include "mongo/util/concurrent_memory_aggregator.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/chunked_memory_aggregator.h"

namespace mongo {

/**
 * Smart pointer deleter instance to handle de-registering a child aggregator from a parent
 * aggregator
 */
class ChunkedMemoryAggregatorDeleter {
public:
    ChunkedMemoryAggregatorDeleter(ConcurrentMemoryAggregator* parent) : _parent(parent) {}

    void operator()(ChunkedMemoryAggregator* child) {
        _parent->remove(child);
        delete child;
    }

private:
    ConcurrentMemoryAggregator* _parent{nullptr};
};  // class ChunkedMemoryAggregatorDeleter

ConcurrentMemoryAggregator::ConcurrentMemoryAggregator(
    std::shared_ptr<ConcurrentMemoryAggregator::UsageMonitor> usageMonitor)
    : _usageMonitor(std::move(usageMonitor)) {}

ConcurrentMemoryAggregator::~ConcurrentMemoryAggregator() {
    // Ensure that all `MemoryUsageHandle` instances have gone out of scope
    // and freed their memory before the `MemoryAggregator` goes out of scope.
    dassert(_curMemoryUsageBytes.load() == 0);

    // Ensure that all `ChunkedMemoryAggregator` instances have gone out of scope
    // and have been removed from the `ConcurrentMemoryAggregator` before the
    // `ConcurrentMemoryAggregator` goes out of scope.
    dassert(_chunkedMemoryAggregators.empty());
}

std::shared_ptr<ChunkedMemoryAggregator> ConcurrentMemoryAggregator::createChunkedMemoryAggregator(
    ChunkedMemoryAggregator::Options options) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    int64_t id = ++_chunkedMemoryAggregatorIDCounter;
    std::shared_ptr<ChunkedMemoryAggregator> agg(
        new ChunkedMemoryAggregator(
            ChunkedMemoryAggregator::PrivateTag{}, std::move(options), id, this),
        ChunkedMemoryAggregatorDeleter(this));
    _chunkedMemoryAggregators.emplace(id, agg);
    return agg;
}

int64_t ConcurrentMemoryAggregator::getCurrentMemoryUsageBytes() const {
    return _curMemoryUsageBytes.load();
}

void ConcurrentMemoryAggregator::add(ChunkedMemoryAggregator* sourceAggregator, int64_t delta) {
    _curMemoryUsageBytes.fetchAndAdd(delta);
    if (delta > 0) {
        // Only notify if the memory usage increased.
        poll(sourceAggregator);
    }
}

void ConcurrentMemoryAggregator::poll(ChunkedMemoryAggregator* sourceAggregator) const {
    if (_usageMonitor) {
        _usageMonitor->onMemoryUsageIncreased(
            _curMemoryUsageBytes.load(), sourceAggregator->getId(), this);
    }
}

void ConcurrentMemoryAggregator::visitAll(
    std::function<void(std::shared_ptr<const ChunkedMemoryAggregator>)> callback) const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    for (const auto& [_, c] : _chunkedMemoryAggregators) {
        auto chunked = c.lock();
        invariant(chunked);
        callback(std::move(chunked));
    }
}

void ConcurrentMemoryAggregator::remove(const ChunkedMemoryAggregator* chunkedMemoryAggregator) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto it = _chunkedMemoryAggregators.find(chunkedMemoryAggregator->getId());
    dassert(it != _chunkedMemoryAggregators.end());
    if (it != _chunkedMemoryAggregators.end()) {
        _chunkedMemoryAggregators.erase(it);
    }
}

};  // namespace mongo

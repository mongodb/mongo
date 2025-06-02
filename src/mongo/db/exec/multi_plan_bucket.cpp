/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/multi_plan_bucket.h"

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
/**
 * The total number of multiplannings allowed to proceed by the rate limiter.
 */
auto& rateLimiterAllowedCount = *MetricBuilder<Counter64>{"query.multiPlanner.rateLimiter.allowed"};

/**
 * The total number of multiplannings delayed but allowed to proceed by the rate limiter.
 */
auto& rateLimiterDelayedCount = *MetricBuilder<Counter64>{"query.multiPlanner.rateLimiter.delayed"};


/**
 * The total number of multiplannings refused by the rate limiter.
 */
auto& rateLimiterRefusedCount = *MetricBuilder<Counter64>{"query.multiPlanner.rateLimiter.refused"};

/**
 * This class defines a table containing the buckets used in the MultiPlan rate limiting algorithm.
 * It maintains a bucket for each plan cache key.
 */
class BucketTable {
public:
    std::shared_ptr<MultiPlanBucket> get(const std::string& key) {
        stdx::lock_guard guard{_mutex};
        auto [pos, inserted] = _table.try_emplace(key);
        if (inserted) {
            pos->second = std::make_shared<MultiPlanBucket>(
                internalQueryMaxConcurrentMultiPlanJobsPerCacheKey.load());
        }
        return pos->second;
    }

    /**
     * Gets a shared pointer to the requested bucket and erases it from the bucket table. Both
     * operations are performed under a single mutex lock for efficiency.
     */
    std::shared_ptr<MultiPlanBucket> getAndErase(const std::string& key) {
        stdx::lock_guard guard{_mutex};
        std::shared_ptr<MultiPlanBucket> result{};
        auto it = _table.find(key);

        if (it != _table.end()) {
            result = it->second;
            _table.erase(it);
        }

        return result;
    }

private:
    stdx::unordered_map<std::string, std::shared_ptr<MultiPlanBucket>> _table;
    stdx::mutex _mutex;
};


/**
 * Copy assignable wrapper around BucketTable required for Collection decoration.
 */
class BucketTablePointer {
public:
    BucketTablePointer() : _table{std::make_shared<BucketTable>()} {}

    std::shared_ptr<MultiPlanBucket> get(const std::string& key) const {
        return _table->get(key);
    }

    std::shared_ptr<MultiPlanBucket> getAndErase(const std::string& key) {
        return _table->getAndErase(key);
    }

private:
    std::shared_ptr<BucketTable> _table;
};

// All buckets stored a collection decoration.
const auto bucketTableDecoration = Collection::declareDecoration<BucketTablePointer>();
}  // namespace

MultiPlanTokens::MultiPlanTokens(MultiPlanTokens&& other) {
    _tokensCount = other._tokensCount;
    other._tokensCount = 0;
    _bucket = std::move(other._bucket);
}

MultiPlanTokens& MultiPlanTokens::operator=(MultiPlanTokens&& other) {
    _tokensCount = other._tokensCount;
    other._tokensCount = 0;
    _bucket = std::move(other._bucket);
    return *this;
}

MultiPlanTokens::~MultiPlanTokens() {
    if (_tokensCount == 0) {
        return;
    }

    auto bucket = _bucket.lock();
    if (bucket) {
        bucket->releaseTokens(_tokensCount);
    }
}

std::shared_ptr<MultiPlanBucket> MultiPlanBucket::get(const std::string& planCacheKey,
                                                      const CollectionPtr& collection) {
    return bucketTableDecoration(collection.get()).get(planCacheKey);
}

void MultiPlanBucket::release(const std::string& planCacheKey, const CollectionPtr& collection) {
    auto table = bucketTableDecoration(collection.get());
    auto bucket = table.getAndErase(planCacheKey);
    if (bucket) {
        bucket->unlockAll();
    }
}

boost::optional<MultiPlanTokens> MultiPlanBucket::getTokens(size_t requestedTokens,
                                                            PlanYieldPolicy* yieldPolicy,
                                                            OperationContext* opCtx) {
    const bool inMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();

    boost::optional<MultiPlanTokens> tokens{};

    stdx::unique_lock guard(_mutex);
    if (_active && _tokens >= requestedTokens) {
        _tokens -= requestedTokens;
        guard.unlock();
        tokens.emplace(requestedTokens, weak_from_this());
        rateLimiterAllowedCount.increment();
        return tokens;
    }

    while (_active && _tokens < requestedTokens &&
           _cv.wait_for(
               guard,
               _yieldingTimeout,
               [this, requestedTokens] { return _tokens >= requestedTokens; }) == false) {
        // Multidocument transactions do not yield.
        if (!inMultiDocumentTransaction) {
            guard.unlock();  // unlock the mutex to avoid deadlocks during yielding
            if (yieldPolicy->shouldYieldOrInterrupt(opCtx)) {
                uassertStatusOK(yieldPolicy->yieldOrInterrupt(
                    opCtx, nullptr, RestoreContext::RestoreType::kYield));
            }
            guard.lock();
        }
    }

    // If the bucket is still active (the plan is not cached yet) we can try to get tokens.
    if (_active && _tokens >= requestedTokens) {
        _tokens -= requestedTokens;
        guard.unlock();
        tokens.emplace(requestedTokens, weak_from_this());
        rateLimiterDelayedCount.increment();
    } else {
        rateLimiterRefusedCount.increment();
    }

    return tokens;
}

void MultiPlanBucket::releaseTokens(size_t tokens) {
    {
        stdx::unique_lock guard(_mutex);
        _tokens += tokens;
    }
    _cv.notify_one();
}

void MultiPlanBucket::unlockAll() {
    {
        stdx::unique_lock guard(_mutex);
        _active = false;
    }
    LOGV2_DEBUG(8712810, 5, "Threads waiting for the cache entry are about to be woken up");
    _cv.notify_all();
}
}  // namespace mongo

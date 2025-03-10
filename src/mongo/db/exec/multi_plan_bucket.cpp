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
 * This class defines a table containing the buckets used in the MultiPlan rate limiting algorithm.
 * It maintains a bucket for each plan cache key.
 */
class BucketTable : public RefCountable {
public:
    boost::intrusive_ptr<MultiPlanBucket> get(const std::string& key) {
        stdx::lock_guard guard{_mutex};
        auto [pos, inserted] = _table.try_emplace(key);
        if (inserted) {
            pos->second = make_intrusive<MultiPlanBucket>(
                internalQueryMaxConcurrentMultiPlanJobsPerCacheKey.load());
        }
        return pos->second;
    }

private:
    stdx::unordered_map<std::string, boost::intrusive_ptr<MultiPlanBucket>> _table;
    stdx::mutex _mutex;
};


/**
 * Copy assignable wrapper around BucketTable required for Collection decoration.
 */
class BucketTablePointer {
public:
    BucketTablePointer() : _table{make_intrusive<BucketTable>()} {}

    boost::intrusive_ptr<MultiPlanBucket> get(const std::string& key) const {
        return _table->get(key);
    }

private:
    boost::intrusive_ptr<BucketTable> _table;
};

// All buckets stored a collection decoration.
const auto bucketTableDecoration = Collection::declareDecoration<BucketTablePointer>();
}  // namespace

MultiPlanTokens::~MultiPlanTokens() {
    _bucket->releaseTokens(_tokensCount);
}

boost::intrusive_ptr<MultiPlanBucket> MultiPlanBucket::get(const std::string& planCacheKey,
                                                           const CollectionPtr& collection) {
    return bucketTableDecoration(collection.get()).get(planCacheKey);
}

boost::optional<MultiPlanTokens> MultiPlanBucket::getTokens(size_t requestedTokens,
                                                            PlanYieldPolicy* yieldPolicy,
                                                            OperationContext* opCtx) {
    const bool inMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();

    stdx::unique_lock guard(_mutex);
    if (_tokens >= requestedTokens) {
        _tokens -= requestedTokens;
        guard.unlock();
        return MultiPlanTokens{requestedTokens, boost::intrusive_ptr<MultiPlanBucket>(this)};
    }

    while (_tokens < requestedTokens &&
           _cv.wait_for(guard, _yieldingTimeout, [this, requestedTokens] {
               return _tokens >= requestedTokens;
           }) == false) {
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

    return boost::none;
}

void MultiPlanBucket::releaseTokens(size_t tokens) {
    {
        stdx::unique_lock guard(_mutex);
        _tokens += tokens;
    }
    _cv.notify_one();
}

void MultiPlanBucket::unlockAll() {
    LOGV2_DEBUG(8712810, 5, "Threads waiting for the cache entry are about to be woken up");
    _cv.notify_all();
}
}  // namespace mongo

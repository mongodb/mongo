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

#pragma once

#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/plan_yield_policy.h"

/**
 * This file defines primitives used in the MultiPlan rate limiting algorithm as well as the
 * algorithm itself. In this algorithm, each plan cache key gets its own bucket. When a query thread
 * attempts to multi-plan the query corresponding to the plan cache key, it asks for one token per
 * candidate plan from the query’s plan cache key bucket.  If the tokens are available, the thread
 * receives them and proceeds. After finishing, it returns the borrowed tokens. If there are not
 * enough tokens in the bucket, the thread waits until it can proceed. Once it resumes, it retries
 * the planning process.
 */

namespace mongo {

class MultiPlanTokens;

/**
 * This class defines a bucket in the Multi Plan rate limiting algorithm. It controls number of
 * concurrent multi plannings for the same query shape.
 */
class MultiPlanBucket : public std::enable_shared_from_this<MultiPlanBucket> {
    static constexpr stdx::chrono::milliseconds _yieldingTimeout{20};

public:
    /**
     * Tokens specify the maximum number of concurrent multiplannings for the same plan cache key.
     */
    explicit MultiPlanBucket(size_t tokens) : _active(true), _tokens(tokens) {}

    template <typename PlanCacheKeyType>
    static std::shared_ptr<MultiPlanBucket> get(const PlanCacheKeyType& planCacheKey,
                                                const CollectionPtr& coll) {
        return get(planCacheKey.toString(), coll);
    }

    static std::shared_ptr<MultiPlanBucket> get(const std::string& planCacheKey,
                                                const CollectionPtr& collection);
    /**
     * Releases the bucket for the given plan cache key: erases it from the bucket table and
     * wakes up all threads that are waiting for this plan cache key. The function should be called
     * when a plan for the plan cache key has been cached.
     */
    static void release(const std::string& planCacheKey, const CollectionPtr& collection);

    /**
     * If enough tokens are available the function returns with it immediately, otherwise it sleeps
     * periodically yielding if required. After the token become available the function returns but
     * without tokens, indicating that the calling function needs to do replanning.
     */
    boost::optional<MultiPlanTokens> getTokens(size_t tokens,
                                               PlanYieldPolicy* yieldPolicy,
                                               OperationContext* opCtx);


    /**
     * This function is called when another thread completes multiplanning and frees tokens.
     */
    void releaseTokens(size_t tokens);

    /**
     * This function is called when a plan for the bucket's plan cache key has been successfully
     * cached.
     */
    void unlockAll();

private:
    bool _active;  // false when the plan is cached
    size_t _tokens;
    stdx::mutex _mutex;
    stdx::condition_variable_any _cv;
};

/**
 * This class defines tokens that each thread must obtain to attempt multiplanning, provided that
 * the MultiPlan rate limiting is enabled. If not enough tokens are available, the thread must wait
 * until either the plan is cached or tokens become available.
 */
class MultiPlanTokens {
public:
    MultiPlanTokens(size_t tokensCount, std::weak_ptr<MultiPlanBucket> bucket)
        : _tokensCount(tokensCount), _bucket(std::move(bucket)) {}

    MultiPlanTokens(const MultiPlanTokens&) = delete;

    MultiPlanTokens(MultiPlanTokens&& other) noexcept
        : _tokensCount(std::exchange(other._tokensCount, 0)),
          _bucket(std::exchange(other._bucket, {})) {}


    MultiPlanTokens& operator=(const MultiPlanTokens&) = delete;

    MultiPlanTokens& operator=(MultiPlanTokens&& other) noexcept {
        if (this != &other) {
            _tokensCount = std::exchange(other._tokensCount, 0);
            _bucket = std::exchange(other._bucket, {});
        }
        return *this;
    }

    ~MultiPlanTokens();

private:
    size_t _tokensCount;
    std::weak_ptr<MultiPlanBucket> _bucket;
};
}  // namespace mongo

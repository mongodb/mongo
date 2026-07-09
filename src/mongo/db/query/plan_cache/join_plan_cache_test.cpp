/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

std::unique_ptr<JoinPlanCacheEntry> makeEntry() {
    return std::make_unique<JoinPlanCacheEntry>(
        nullptr, join_ordering::NodeId{0}, std::vector<CollectionTag>{});
}

TEST(JoinPlanCacheTest, LookupOnEmptyCacheReturnsNull) {
    JoinPlanCache cache;
    ASSERT_EQ(nullptr, cache.lookup("key"));
}

TEST(JoinPlanCacheTest, PutAndLookupRoundtrip) {
    JoinPlanCache cache;
    auto entry = makeEntry();
    auto rawPtr = entry.get();
    cache.put("key", std::move(entry));
    ASSERT_EQ(rawPtr, cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, PutOverwritesExistingEntry) {
    JoinPlanCache cache;
    cache.put("key", makeEntry());

    auto newEntry = makeEntry();
    const JoinPlanCacheEntry* newRawPtr = newEntry.get();
    cache.put("key", std::move(newEntry));

    auto result = cache.lookup("key");
    ASSERT_EQ(newRawPtr, result.get());
}

TEST(JoinPlanCacheTest, RemoveExistingEntry) {
    JoinPlanCache cache;
    cache.put("key", makeEntry());
    cache.remove("key");
    ASSERT_EQ(nullptr, cache.lookup("key"));
}

TEST(JoinPlanCacheTest, RemoveNonExistingEntry) {
    JoinPlanCache cache;
    auto entry = makeEntry();
    const JoinPlanCacheEntry* rawPtr = entry.get();
    cache.put("key", std::move(entry));
    cache.remove("nonexistent");
    ASSERT_EQ(rawPtr, cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, GetComplexEntry) {
    JoinPlanCache cache;

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedJoinNode{
            .method = join_ordering::JoinMethod::HJ,
            .joinPredicates = {QSNJoinPredicate{
                .op = QSNJoinPredicate::ComparisonOp::Eq,
                .leftField = FieldPath("foo"),
                .rightField = FieldPath("bar"),
            }},
            .left = std::make_unique<CachedJoinPlan>(CachedAccessPath{
                .nodeId = 0,
                .solnCacheData = std::make_unique<SolutionCacheData>(SolutionCacheData{}),
            }),
            .right = std::make_unique<CachedJoinPlan>(CachedAccessPath{
                .nodeId = 1,
                .solnCacheData = std::make_unique<SolutionCacheData>(SolutionCacheData{}),
            }),
        }),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{});

    const JoinPlanCacheEntry* rawPtr = entry.get();
    cache.put("key", std::move(entry));

    auto result = cache.lookup("key");
    ASSERT_EQ(rawPtr, result.get());
}

}  // namespace
}  // namespace mongo

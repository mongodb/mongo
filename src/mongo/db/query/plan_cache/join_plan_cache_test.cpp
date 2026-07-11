// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

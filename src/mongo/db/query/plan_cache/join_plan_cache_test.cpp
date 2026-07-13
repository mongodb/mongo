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

std::unique_ptr<CachedJoinPlan> makeComplexTree() {
    return std::make_unique<CachedJoinPlan>(CachedJoinNode{
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
    });
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
        makeComplexTree(), join_ordering::NodeId{0}, std::vector<CollectionTag>{});
    const JoinPlanCacheEntry* rawPtr = entry.get();
    cache.put("key", std::move(entry));

    auto result = cache.lookup("key");
    ASSERT_EQ(rawPtr, result.get());
}

TEST(JoinPlanCacheSizeTest, TrivialEntrySizeIsSizeofEntry) {
    auto entry = makeEntry();
    ASSERT_EQ(sizeof(JoinPlanCacheEntry), entry->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, AccessPathLeafSize) {
    auto entry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedAccessPath{
            .nodeId = 0,
            .solnCacheData = std::make_unique<SolutionCacheData>(SolutionCacheData{}),
        }),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{});

    // A default SolutionCacheData has a null tree, so its footprint is just its own sizeof.
    const size_t expected =
        sizeof(JoinPlanCacheEntry) + sizeof(CachedJoinPlan) + sizeof(SolutionCacheData);
    ASSERT_EQ(expected, entry->estimatedEntrySizeBytes);
    ASSERT_GT(entry->estimatedEntrySizeBytes, makeEntry()->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, ComplexTreeLargerThanLeaf) {
    auto complexEntry = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(), join_ordering::NodeId{0}, std::vector<CollectionTag>{});
    auto leafEntry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedAccessPath{
            .nodeId = 0,
            .solnCacheData = std::make_unique<SolutionCacheData>(SolutionCacheData{}),
        }),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{});

    // The complex tree contains two access-path leaves plus a join node and predicate, so it must
    // be strictly larger than a single leaf.
    ASSERT_GT(complexEntry->estimatedEntrySizeBytes, leafEntry->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, LongerFieldPathIncreasesSize) {
    auto baseline = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(), join_ordering::NodeId{0}, std::vector<CollectionTag>{});

    auto longFieldTree = makeComplexTree();
    std::get<CachedJoinNode>(longFieldTree->node).joinPredicates[0].leftField =
        FieldPath("a.very.long.dotted.field.path.that.uses.more.heap.storage");
    auto longFieldEntry = std::make_unique<JoinPlanCacheEntry>(
        std::move(longFieldTree), join_ordering::NodeId{0}, std::vector<CollectionTag>{});

    ASSERT_GT(longFieldEntry->estimatedEntrySizeBytes, baseline->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, AdditionalPredicateIncreasesSize) {
    auto baseline = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(), join_ordering::NodeId{0}, std::vector<CollectionTag>{});

    auto extraPredTree = makeComplexTree();
    std::get<CachedJoinNode>(extraPredTree->node)
        .joinPredicates.push_back(QSNJoinPredicate{
            .op = QSNJoinPredicate::ComparisonOp::Eq,
            .leftField = FieldPath("baz"),
            .rightField = FieldPath("qux"),
        });
    auto extraPredEntry = std::make_unique<JoinPlanCacheEntry>(
        std::move(extraPredTree), join_ordering::NodeId{0}, std::vector<CollectionTag>{});

    ASSERT_GT(extraPredEntry->estimatedEntrySizeBytes, baseline->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, LongerInljIndexNameIncreasesSize) {
    auto shortEntry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedInljNode{.nodeId = 0, .inljForeignIndexName = "ix"}),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{});
    auto longEntry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedInljNode{
            .nodeId = 0,
            .inljForeignIndexName = "a_much_longer_foreign_index_name_that_exceeds_sso"}),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{});

    ASSERT_GT(longEntry->estimatedEntrySizeBytes, shortEntry->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, BudgetEstimatorSumsEntryAndKey) {
    JoinPlanCacheBudgetEstimator estimator;
    std::shared_ptr<const JoinPlanCacheEntry> entry = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(), join_ordering::NodeId{0}, std::vector<CollectionTag>{});

    const JoinPlanCacheKey key = "some-encoded-shape";
    ASSERT_EQ(entry->estimatedEntrySizeBytes + key.size(), estimator(key, entry));

    // A longer key contributes more budget.
    const JoinPlanCacheKey longerKey = key + "-with-extra-discriminators";
    ASSERT_GT(estimator(longerKey, entry), estimator(key, entry));
}

TEST(JoinPlanCacheSizeTest, EstimateIsDeterministic) {
    auto entry = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(), join_ordering::NodeId{0}, std::vector<CollectionTag>{});
    ASSERT_EQ(entry->joinTree->estimateObjectSizeInBytes(),
              entry->joinTree->estimateObjectSizeInBytes());
}

}  // namespace
}  // namespace mongo

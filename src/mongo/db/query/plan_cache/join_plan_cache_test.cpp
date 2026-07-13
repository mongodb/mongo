// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// A budget large enough that no entry used in these tests is ever evicted.
constexpr size_t kLargeBudget = size_t{1} << 30;

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

JoinPlanCache largeBudgetSinglePartitionCache() {
    return JoinPlanCache{kLargeBudget, 1};
}

TEST(JoinPlanCacheTest, LookupOnEmptyCacheReturnsNull) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    ASSERT_EQ(nullptr, cache.lookup("key"));
}

TEST(JoinPlanCacheTest, PutAndLookupRoundtrip) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    auto entry = makeEntry();
    auto rawPtr = entry.get();
    cache.put("key", std::move(entry));
    ASSERT_EQ(rawPtr, cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, PutOverwritesExistingEntry) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key", makeEntry());

    auto newEntry = makeEntry();
    const JoinPlanCacheEntry* newRawPtr = newEntry.get();
    cache.put("key", std::move(newEntry));

    auto result = cache.lookup("key");
    ASSERT_EQ(newRawPtr, result.get());
}

TEST(JoinPlanCacheTest, RemoveExistingEntry) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key", makeEntry());
    cache.remove("key");
    ASSERT_EQ(nullptr, cache.lookup("key"));
}

TEST(JoinPlanCacheTest, RemoveNonExistingEntry) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    auto entry = makeEntry();
    const JoinPlanCacheEntry* rawPtr = entry.get();
    cache.put("key", std::move(entry));
    cache.remove("nonexistent");
    ASSERT_EQ(rawPtr, cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, GetComplexEntry) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();

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

// The per-entry budget cost of a trivial entry under a key of the given length: the entry's own
// estimated size plus the key string length (see JoinPlanCacheBudgetEstimator).
size_t trivialEntryCost(size_t keyLength) {
    return makeEntry()->estimatedEntrySizeBytes + keyLength;
}

TEST(JoinPlanCacheEvictionTest, InsertingBeyondBudgetEvictsLeastRecentlyUsed) {
    const size_t perEntryCost = trivialEntryCost(1);
    // Budget for exactly one entry and one partition
    JoinPlanCache cache{perEntryCost, 1};

    ASSERT_EQ(0, cache.put("a", makeEntry()));
    // Evicts "a"
    auto bEntry = makeEntry();
    auto bRawPtr = bEntry.get();
    ASSERT_EQ(1, cache.put("b", std::move(bEntry)));

    ASSERT_EQ(nullptr, cache.lookup("a"));
    ASSERT_EQ(bRawPtr, cache.lookup("b").get());
}

TEST(JoinPlanCacheEvictionTest, LookupPromotesEntryToMostRecentlyUsed) {
    const size_t perEntryCost = trivialEntryCost(1);
    // Budget for two entries and one partition
    JoinPlanCache cache{2 * perEntryCost, 1};

    auto aEntry = makeEntry();
    auto aRawPtr = aEntry.get();
    cache.put("a", std::move(aEntry));
    cache.put("b", makeEntry());
    // Promotes "a" ahead of "b"
    ASSERT_EQ(aRawPtr, cache.lookup("a").get());
    // Over budget causes LRU eviction, which is now "b"
    auto cEntry = makeEntry();
    auto cRawPtr = cEntry.get();
    cache.put("c", std::move(cEntry));

    ASSERT_EQ(aRawPtr, cache.lookup("a").get());
    ASSERT_EQ(nullptr, cache.lookup("b"));
    ASSERT_EQ(cRawPtr, cache.lookup("c").get());
}

TEST(JoinPlanCacheEvictionTest, ResetToSmallerBudgetEvictsDownToFit) {
    const size_t perEntryCost = trivialEntryCost(1);
    JoinPlanCache cache{3 * perEntryCost, 1};

    cache.put("a", makeEntry());
    cache.put("b", makeEntry());
    auto cEntry = makeEntry();
    auto cRawPtr = cEntry.get();
    cache.put("c", std::move(cEntry));
    ASSERT_EQ(3 * perEntryCost, cache.size());

    // "c" is the most recently used, so "a" and "b" are evicted to fit the new budget.
    ASSERT_EQ(2, cache.reset(perEntryCost));
    ASSERT_EQ(perEntryCost, cache.size());
    ASSERT_EQ(nullptr, cache.lookup("a"));
    ASSERT_EQ(nullptr, cache.lookup("b"));
    ASSERT_EQ(cRawPtr, cache.lookup("c").get());
}

TEST(JoinPlanCacheEvictionTest, SizeReflectsRunningByteTotal) {
    const size_t perEntryCost = trivialEntryCost(1);
    JoinPlanCache cache{kLargeBudget, 1};

    ASSERT_EQ(0, cache.size());
    cache.put("a", makeEntry());
    ASSERT_EQ(perEntryCost, cache.size());
    cache.put("b", makeEntry());
    ASSERT_EQ(2 * perEntryCost, cache.size());
    cache.remove("a");
    ASSERT_EQ(perEntryCost, cache.size());
}

}  // namespace
}  // namespace mongo

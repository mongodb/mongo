// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/join/join_method.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// A budget large enough that no entry used in these tests is ever evicted.
constexpr size_t kLargeBudget = size_t{1} << 30;

std::unique_ptr<JoinPlanCacheEntry> makeEntry(JoinPlanBaseline baseline = {}) {
    return std::make_unique<JoinPlanCacheEntry>(nullptr,
                                                join_ordering::NodeId{0},
                                                std::vector<CollectionTag>{},
                                                std::vector<NodeFingerprint>{},
                                                baseline);
}

std::unique_ptr<CachedJoinPlan> makeComplexTree(
    join_ordering::JoinMethod method = join_ordering::JoinMethod::HJ) {
    CachedJoinNode node{
        .method = method,
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
    };
    if (method == join_ordering::JoinMethod::INLJ) {
        node.probePredicates = {QSNJoinPredicate{
            .op = QSNJoinPredicate::ComparisonOp::Eq,
            .leftField = FieldPath("foo"),
            .rightField = FieldPath("baz"),
        }};
    }
    return std::make_unique<CachedJoinPlan>(std::move(node));
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

TEST(JoinPlanCacheTest, RemoveIfMatchesRemovesTheEntryItWasGiven) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key", makeEntry());

    auto held = cache.lookup("key");
    ASSERT_TRUE(cache.removeIfMatches("key", held));
    ASSERT_EQ(nullptr, cache.lookup("key"));
}

TEST(JoinPlanCacheTest, RemoveIfMatchesLeavesAnEntryReplacedSinceTheLookup) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key", makeEntry());

    // Store pointer to the current entry.
    auto held = cache.lookup("key");

    // Replace entry in the cache.
    auto replacement = makeEntry();
    const JoinPlanCacheEntry* replacementRawPtr = replacement.get();
    cache.put("key", std::move(replacement));

    // Removal of entry should not occur since 'held' is stale.
    ASSERT_FALSE(cache.removeIfMatches("key", held));
    ASSERT_EQ(replacementRawPtr, cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, RemoveIfMatchesOnNonExistingKey) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key", makeEntry());
    auto held = cache.lookup("key");

    ASSERT_FALSE(cache.removeIfMatches("nonexistent", held));
    ASSERT_EQ(held.get(), cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, RemoveIfMatchesComparesIdentityNotContents) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    auto entry = makeEntry();
    const JoinPlanCacheEntry* rawPtr = entry.get();
    cache.put("key", std::move(entry));

    // A distinct entry that has the same contents as the original entry.
    std::shared_ptr<JoinPlanCacheEntry> lookalike = makeEntry();
    ASSERT_FALSE(cache.removeIfMatches("key", lookalike));
    ASSERT_EQ(rawPtr, cache.lookup("key").get());
}

TEST(JoinPlanCacheTest, GetComplexEntry) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();

    const JoinPlanBaseline baseline{
        .numReads = 5, .numIntermediateRows = 9, .spilled = true, .numResults = 2};
    auto entry = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                      join_ordering::NodeId{0},
                                                      std::vector<CollectionTag>{},
                                                      std::vector<NodeFingerprint>{},
                                                      baseline);
    const JoinPlanCacheEntry* rawPtr = entry.get();
    cache.put("key", std::move(entry));

    auto result = cache.lookup("key");
    ASSERT_EQ(rawPtr, result.get());
    ASSERT_BSONOBJ_EQ(baseline.toBSON(), result->baseline.toBSON());
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
        std::vector<CollectionTag>{},
        std::vector<NodeFingerprint>{},
        JoinPlanBaseline{});

    // A default SolutionCacheData has a null tree, so its footprint is just its own sizeof.
    const size_t expected =
        sizeof(JoinPlanCacheEntry) + sizeof(CachedJoinPlan) + sizeof(SolutionCacheData);
    ASSERT_EQ(expected, entry->estimatedEntrySizeBytes);
    ASSERT_GT(entry->estimatedEntrySizeBytes, makeEntry()->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, ComplexTreeLargerThanLeaf) {
    auto complexEntry = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                             join_ordering::NodeId{0},
                                                             std::vector<CollectionTag>{},
                                                             std::vector<NodeFingerprint>{},
                                                             JoinPlanBaseline{});
    auto leafEntry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedAccessPath{
            .nodeId = 0,
            .solnCacheData = std::make_unique<SolutionCacheData>(SolutionCacheData{}),
        }),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{},
        std::vector<NodeFingerprint>{},
        JoinPlanBaseline{});

    // The complex tree contains two access-path leaves plus a join node and predicate, so it must
    // be strictly larger than a single leaf.
    ASSERT_GT(complexEntry->estimatedEntrySizeBytes, leafEntry->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, RelevantIndexHashesIncreaseSize) {
    auto baseline =
        std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                             join_ordering::NodeId{0},
                                             std::vector<CollectionTag>{},
                                             std::vector<NodeFingerprint>{NodeFingerprint{}},
                                             JoinPlanBaseline{});

    auto withHashes = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{},
        std::vector<NodeFingerprint>{NodeFingerprint{.relevantIndexHashes = {2, 3, 4}}},
        JoinPlanBaseline{});

    ASSERT_GT(withHashes->estimatedEntrySizeBytes, baseline->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, LongerFieldPathIncreasesSize) {
    auto baseline = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                         join_ordering::NodeId{0},
                                                         std::vector<CollectionTag>{},
                                                         std::vector<NodeFingerprint>{},
                                                         JoinPlanBaseline{});

    auto longFieldTree = makeComplexTree();
    std::get<CachedJoinNode>(longFieldTree->node).joinPredicates[0].leftField =
        FieldPath("a.very.long.dotted.field.path.that.uses.more.heap.storage");
    auto longFieldEntry = std::make_unique<JoinPlanCacheEntry>(std::move(longFieldTree),
                                                               join_ordering::NodeId{0},
                                                               std::vector<CollectionTag>{},
                                                               std::vector<NodeFingerprint>{},
                                                               JoinPlanBaseline{});

    ASSERT_GT(longFieldEntry->estimatedEntrySizeBytes, baseline->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, AdditionalPredicateIncreasesSize) {
    auto baseline =
        std::make_unique<JoinPlanCacheEntry>(makeComplexTree(join_ordering::JoinMethod::INLJ),
                                             join_ordering::NodeId{0},
                                             std::vector<CollectionTag>{},
                                             std::vector<NodeFingerprint>{},
                                             JoinPlanBaseline{});

    // Validate join predicates contribute to size.
    auto extraJoinPredTree = makeComplexTree(join_ordering::JoinMethod::INLJ);
    std::get<CachedJoinNode>(extraJoinPredTree->node)
        .joinPredicates.push_back(QSNJoinPredicate{
            .op = QSNJoinPredicate::ComparisonOp::Eq,
            .leftField = FieldPath("baz"),
            .rightField = FieldPath("qux"),
        });
    auto extraJoinPredEntry = std::make_unique<JoinPlanCacheEntry>(std::move(extraJoinPredTree),
                                                                   join_ordering::NodeId{0},
                                                                   std::vector<CollectionTag>{},
                                                                   std::vector<NodeFingerprint>{},
                                                                   JoinPlanBaseline{});
    ASSERT_GT(extraJoinPredEntry->estimatedEntrySizeBytes, baseline->estimatedEntrySizeBytes);

    // Validate probe predicates contribute to size.
    auto extraPredTree = makeComplexTree(join_ordering::JoinMethod::INLJ);
    std::get<CachedJoinNode>(extraPredTree->node)
        .joinPredicates.push_back(QSNJoinPredicate{
            .op = QSNJoinPredicate::ComparisonOp::Eq,
            .leftField = FieldPath("baz"),
            .rightField = FieldPath("qux"),
        });
    std::get<CachedJoinNode>(extraPredTree->node)
        .probePredicates.push_back(QSNJoinPredicate{
            .op = QSNJoinPredicate::ComparisonOp::Eq,
            .leftField = FieldPath("baz"),
            .rightField = FieldPath("qux"),
        });
    auto extraPredEntry = std::make_unique<JoinPlanCacheEntry>(std::move(extraPredTree),
                                                               join_ordering::NodeId{0},
                                                               std::vector<CollectionTag>{},
                                                               std::vector<NodeFingerprint>{},
                                                               JoinPlanBaseline{});

    ASSERT_GT(extraPredEntry->estimatedEntrySizeBytes, extraJoinPredEntry->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, LongerInljIndexNameIncreasesSize) {
    auto shortEntry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedInljNode{.nodeId = 0, .inljForeignIndexName = "ix"}),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{},
        std::vector<NodeFingerprint>{},
        JoinPlanBaseline{});
    auto longEntry = std::make_unique<JoinPlanCacheEntry>(
        std::make_unique<CachedJoinPlan>(CachedInljNode{
            .nodeId = 0,
            .inljForeignIndexName = "a_much_longer_foreign_index_name_that_exceeds_sso"}),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{},
        std::vector<NodeFingerprint>{},
        JoinPlanBaseline{});

    ASSERT_GT(longEntry->estimatedEntrySizeBytes, shortEntry->estimatedEntrySizeBytes);
}

TEST(JoinPlanCacheSizeTest, NodeFingerprintsIncreaseSize) {
    auto baseline = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                         join_ordering::NodeId{0},
                                                         std::vector<CollectionTag>{},
                                                         std::vector<NodeFingerprint>{},
                                                         JoinPlanBaseline{});
    auto fingerprinted = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{},
        std::vector<NodeFingerprint>{NodeFingerprint{.relevantIndexHashes = {1}},
                                     NodeFingerprint{.relevantIndexHashes = {2}}},
        JoinPlanBaseline{});

    ASSERT_GTE(fingerprinted->estimatedEntrySizeBytes - baseline->estimatedEntrySizeBytes,
               2 * sizeof(NodeFingerprint));
}

TEST(JoinPlanCacheSizeTest, CollectionTagsIncreaseSize) {
    auto baseline = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                         join_ordering::NodeId{0},
                                                         std::vector<CollectionTag>{},
                                                         std::vector<NodeFingerprint>{},
                                                         JoinPlanBaseline{});
    auto tagged = std::make_unique<JoinPlanCacheEntry>(
        makeComplexTree(),
        join_ordering::NodeId{0},
        std::vector<CollectionTag>{CollectionTag{UUID::gen(), CollectionVersionTag{}}},
        std::vector<NodeFingerprint>{},
        JoinPlanBaseline{});

    ASSERT_GTE(tagged->estimatedEntrySizeBytes - baseline->estimatedEntrySizeBytes,
               sizeof(CollectionTag));
}

TEST(JoinPlanCacheSizeTest, BudgetEstimatorSumsEntryAndKey) {
    JoinPlanCacheBudgetEstimator estimator;
    std::shared_ptr<JoinPlanCacheEntry> entry =
        std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                             join_ordering::NodeId{0},
                                             std::vector<CollectionTag>{},
                                             std::vector<NodeFingerprint>{},
                                             JoinPlanBaseline{});

    const JoinPlanCacheKey key = "some-encoded-shape";
    ASSERT_EQ(entry->estimatedEntrySizeBytes + key.size(), estimator(key, entry));

    // A longer key contributes more budget.
    const JoinPlanCacheKey longerKey = key + "-with-extra-discriminators";
    ASSERT_GT(estimator(longerKey, entry), estimator(key, entry));
}

TEST(JoinPlanCacheSizeTest, EstimateIsDeterministic) {
    auto entry = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                      join_ordering::NodeId{0},
                                                      std::vector<CollectionTag>{},
                                                      std::vector<NodeFingerprint>{},
                                                      JoinPlanBaseline{});
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

TEST(JoinPlanCacheClearTest, ClearEmptyCacheIsNoop) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.clear();
    ASSERT_EQ(0, cache.size());
    ASSERT_TRUE(cache.serializeEntries().empty());
}

TEST(JoinPlanCacheClearTest, ClearRemovesAllEntries) {
    // Use several partitions so that the entries are spread across more than one of them.
    JoinPlanCache cache{kLargeBudget, 4};
    for (const char* key : {"a", "b", "c", "d", "e"}) {
        cache.put(key, makeEntry());
    }
    ASSERT_GT(cache.size(), 0);
    ASSERT_EQ(5u, cache.serializeEntries().size());

    cache.clear();
    ASSERT_EQ(0, cache.size());
    ASSERT_TRUE(cache.serializeEntries().empty());
}

TEST(JoinPlanCacheClearTest, LookupMissesAfterClear) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key", makeEntry());
    ASSERT_NE(nullptr, cache.lookup("key"));

    cache.clear();
    ASSERT_EQ(nullptr, cache.lookup("key"));
}

TEST(JoinPlanCacheClearTest, PutWorksAfterClear) {
    const size_t perEntryCost = trivialEntryCost(1);
    // A budget for exactly one entry: if clear() failed to release the budget, this put() would
    // evict rather than insert cleanly.
    JoinPlanCache cache{perEntryCost, 1};
    cache.put("a", makeEntry());
    cache.clear();

    auto entry = makeEntry();
    const JoinPlanCacheEntry* rawPtr = entry.get();
    ASSERT_EQ(0, cache.put("b", std::move(entry)));
    ASSERT_EQ(rawPtr, cache.lookup("b").get());
    ASSERT_EQ(perEntryCost, cache.size());
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

TEST(JoinPlanCacheEvictionTest, RemoveIfMatchesReleasesTheEntryBudget) {
    const size_t perEntryCost = trivialEntryCost(1);
    JoinPlanCache cache{kLargeBudget, 1};

    cache.put("a", makeEntry());
    ASSERT_EQ(perEntryCost, cache.size());

    // A no-op removal must not disturb the accounting either.
    ASSERT_FALSE(cache.removeIfMatches("a", makeEntry()));
    ASSERT_EQ(perEntryCost, cache.size());

    ASSERT_TRUE(cache.removeIfMatches("a", cache.lookup("a")));
    ASSERT_EQ(0, cache.size());
}

TEST(JoinPlanCacheStatsTest, EmptyCacheProducesNoStats) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    ASSERT_TRUE(cache.serializeEntries().empty());
}

TEST(JoinPlanCacheStatsTest, StatsHaveOneEntryPerCachedPlan) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("keyA", makeEntry());
    cache.put("keyB", makeEntry());

    auto stats = cache.serializeEntries();
    ASSERT_EQ(2u, stats.size());
    for (const auto& obj : stats) {
        ASSERT_TRUE(obj.hasField("planCacheKey"));
        ASSERT_TRUE(obj.hasField("baseNode"));
        ASSERT_TRUE(obj.hasField("estimatedSizeBytes"));
        ASSERT_TRUE(obj.hasField("collections"));
        ASSERT_TRUE(obj.hasField("baseline"));
    }
}

TEST(JoinPlanCacheStatsTest, StatsIncludeEntryBaseline) {
    JoinPlanCache cache = largeBudgetSinglePartitionCache();
    cache.put("key",
              makeEntry(JoinPlanBaseline{
                  .numReads = 11, .numIntermediateRows = 22, .spilled = true, .numResults = 101}));

    auto stats = cache.serializeEntries();
    ASSERT_EQ(1u, stats.size());
    BSONObj baseline = stats[0]["baseline"].Obj();
    ASSERT_EQ(11, baseline["numReads"].numberLong());
    ASSERT_EQ(22, baseline["numIntermediateRows"].numberLong());
    ASSERT_EQ(true, baseline["spilled"].boolean());
    ASSERT_EQ(101, baseline["numResults"].numberLong());
}

TEST(JoinPlanCacheStatsTest, SerializesBaseNodeAndSizeAndKeyHash) {
    auto entry = std::make_unique<JoinPlanCacheEntry>(nullptr,
                                                      join_ordering::NodeId{7},
                                                      std::vector<CollectionTag>{},
                                                      std::vector<NodeFingerprint>{},
                                                      JoinPlanBaseline{});
    const auto expectedSize = static_cast<long long>(entry->estimatedEntrySizeBytes);

    BSONObj obj = joinPlanCacheEntryToBSON("some-encoded-shape", *entry);
    ASSERT_EQ(7, obj["baseNode"].numberInt());
    ASSERT_EQ(expectedSize, obj["estimatedSizeBytes"].numberLong());
    // The key hash is rendered as a fixed-width, zero-padded hex string.
    ASSERT_EQ(BSONType::string, obj["planCacheKey"].type());
    ASSERT_EQ(8, obj["planCacheKey"].str().size());
    // A null join tree omits the plan sub-object.
    ASSERT_FALSE(obj.hasField("plan"));
}

TEST(JoinPlanCacheStatsTest, SerializesComplexPlanTree) {
    auto entry = std::make_unique<JoinPlanCacheEntry>(makeComplexTree(),
                                                      join_ordering::NodeId{0},
                                                      std::vector<CollectionTag>{},
                                                      std::vector<NodeFingerprint>{},
                                                      JoinPlanBaseline{});

    BSONObj obj = joinPlanCacheEntryToBSON("key", *entry);
    ASSERT_TRUE(obj.hasField("plan"));
    BSONObj plan = obj["plan"].Obj();
    ASSERT_EQ(join_ordering::joinMethodToString(join_ordering::JoinMethod::HJ),
              plan["joinMethod"].str());
    ASSERT_EQ(BSONType::array, plan["joinPredicates"].type());
    ASSERT_EQ(1, plan["joinPredicates"].Array().size());
    ASSERT_FALSE(plan.hasField("probePredicates"));
    // Both children are serialized recursively as access-path leaves.
    ASSERT_TRUE(plan["left"].Obj().hasField("nodeId"));
    ASSERT_TRUE(plan["right"].Obj().hasField("nodeId"));
}

TEST(JoinPlanCacheStatsTest, SerializesComplexInljPlanTree) {
    auto entry =
        std::make_unique<JoinPlanCacheEntry>(makeComplexTree(join_ordering::JoinMethod::INLJ),
                                             join_ordering::NodeId{0},
                                             std::vector<CollectionTag>{},
                                             std::vector<NodeFingerprint>{},
                                             JoinPlanBaseline{});

    BSONObj obj = joinPlanCacheEntryToBSON("key", *entry);
    ASSERT_TRUE(obj.hasField("plan"));
    BSONObj plan = obj["plan"].Obj();
    ASSERT_EQ(join_ordering::joinMethodToString(join_ordering::JoinMethod::INLJ),
              plan["joinMethod"].str());
    ASSERT_EQ(BSONType::array, plan["joinPredicates"].type());
    ASSERT_EQ(1, plan["joinPredicates"].Array().size());
    ASSERT_EQ(BSONType::array, plan["probePredicates"].type());
    ASSERT_EQ(1, plan["probePredicates"].Array().size());
    // Both children are serialized recursively as access-path leaves.
    ASSERT_TRUE(plan["left"].Obj().hasField("nodeId"));
    ASSERT_TRUE(plan["right"].Obj().hasField("nodeId"));
}

TEST(JoinPlanCacheStatsTest, SerializesCollectionTags) {
    const auto uuid = UUID::gen();
    std::vector<CollectionTag> tags{
        CollectionTag{uuid, CollectionVersionTag{.collectionVersion = 3, .sampleVersion = 5}}};
    auto entry = std::make_unique<JoinPlanCacheEntry>(nullptr,
                                                      join_ordering::NodeId{0},
                                                      std::move(tags),
                                                      std::vector<NodeFingerprint>{},
                                                      JoinPlanBaseline{});

    BSONObj obj = joinPlanCacheEntryToBSON("key", *entry);
    auto collections = obj["collections"].Array();
    ASSERT_EQ(1, collections.size());
    BSONObj tag = collections[0].Obj();
    ASSERT_EQ(uuid, unittest::assertGet(UUID::parse(tag["uuid"])));
    ASSERT_EQ(3, tag["collectionVersion"].numberLong());
    ASSERT_EQ(5, tag["sampleVersion"].numberLong());
}

TEST(JoinPlanCacheStatsTest, SerializesBaseline) {
    auto entry = makeEntry(JoinPlanBaseline{
        .numReads = 42, .numIntermediateRows = 7, .spilled = true, .numResults = 3});

    BSONObj baseline = joinPlanCacheEntryToBSON("key", *entry)["baseline"].Obj();
    ASSERT_EQ(42, baseline["numReads"].numberLong());
    ASSERT_EQ(7, baseline["numIntermediateRows"].numberLong());
    ASSERT_EQ(true, baseline["spilled"].boolean());
    ASSERT_EQ(3, baseline["numResults"].numberLong());
}

TEST(JoinPlanCacheStatsTest, DefaultBaselineIsZeros) {
    BSONObj baseline = joinPlanCacheEntryToBSON("key", *makeEntry())["baseline"].Obj();
    ASSERT_EQ(0, baseline["numReads"].numberLong());
    ASSERT_EQ(0, baseline["numIntermediateRows"].numberLong());
    ASSERT_EQ(false, baseline["spilled"].boolean());
    ASSERT_EQ(0, baseline["numResults"].numberLong());
}

}  // namespace
}  // namespace mongo

// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_join_plan_cache_stats.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// RAII helper that enables the join optimization and join plan cache knobs for the duration of a
// test and restores their prior values afterwards.
template <bool enableJoin>
class DocumentSourceJoinPlanCacheStatsTest : public AggregationContextFixture {
public:
    DocumentSourceJoinPlanCacheStatsTest()
        : _joinOptGuard{"internalEnableJoinOptimization", enableJoin},
          _joinPlanCacheGuard{"internalEnableJoinPlanCache", enableJoin} {}

private:
    unittest::ServerParameterGuard _joinOptGuard;
    unittest::ServerParameterGuard _joinPlanCacheGuard;
};

// A process interface that reports it is expected to execute queries, so that router parsing is
// exercised the same way it is in a real cluster rather than as a parse-only reparse.
class ExecutingMongoProcessInterface final : public StubMongoProcessInterface {
public:
    bool isExpectedToExecuteQueries() override {
        return true;
    }
};

constexpr auto kShardName = "shard0";

// A process interface which reports a shard name, as a real shard's would.
class ShardMongoProcessInterface final : public StubMongoProcessInterface {
public:
    std::string getShardName(OperationContext*) const override {
        return kShardName;
    }
};

std::unique_ptr<JoinPlanCacheEntry> makeCacheEntry() {
    return std::make_unique<JoinPlanCacheEntry>(nullptr,
                                                join_ordering::NodeId{0},
                                                std::vector<CollectionTag>{},
                                                std::vector<NodeFingerprint>{});
}

NamespaceString adminCollectionlessNss() {
    return NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);
}

const BSONObj kEmptySpecObj = fromjson("{$joinPlanCacheStats: {}}");

using DocumentSourceJoinPlanCacheStatsTestCacheDisabled =
    DocumentSourceJoinPlanCacheStatsTest<false>;

using DocumentSourceJoinPlanCacheStatsTestCacheEnabled = DocumentSourceJoinPlanCacheStatsTest<true>;

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheDisabled, ShouldFailToParseWhenKnobsDisabled) {
    // Knobs default to disabled; the stage should reject regardless of namespace.
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    ASSERT_THROWS_CODE(
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::QueryFeatureNotAllowed);
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, ShouldFailToParseIfSpecIsNotObject) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    const auto specObj = fromjson("{$joinPlanCacheStats: 1}");
    ASSERT_THROWS_CODE(
        DocumentSourceJoinPlanCacheStats::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, ShouldFailToParseIfSpecIsNonEmptyObject) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    const auto specObj = fromjson("{$joinPlanCacheStats: {unknownOption: 1}}");
    ASSERT_THROWS_CODE(
        DocumentSourceJoinPlanCacheStats::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, ShouldFailToParseAgainstACollection) {
    // The fixture's default namespace is a normal collection (not a collectionless admin agg).
    ASSERT_THROWS_CODE(
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, ShouldFailToParseAgainstNonAdminDatabase) {
    getExpCtx()->setNamespaceString(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kLocal));
    ASSERT_THROWS_CODE(
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidNamespace);
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, CanParseOnRouter) {
    // On a router the stage parses normally; it is dispatched to the shards for execution.
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    getExpCtx()->setInRouter(true);
    getExpCtx()->setMongoProcessInterface(std::make_shared<ExecutingMongoProcessInterface>());
    auto stage =
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    ASSERT(stage);
    std::vector<Value> serialized;
    stage->serializeToArray(serialized);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(kEmptySpecObj, serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, TargetsAllShardsAndIsNeverSplit) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    auto stage =
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto constraints = stage->constraints();
    ASSERT_TRUE(constraints.hostRequirement ==
                StageConstraints::HostTypeRequirement::kTargetedShards);

    // The stage stays wholly in the shards pipeline; the router simply unions the shard cursors.
    ASSERT_FALSE(stage->distributedPlanLogic(nullptr).has_value());
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, CanParseAndSerializeSuccessfully) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    auto stage =
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    stage->serializeToArray(serialized);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(kEmptySpecObj, serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, IsCollectionlessFirstStage) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    auto stage =
        DocumentSourceJoinPlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto constraints = stage->constraints();
    ASSERT_TRUE(constraints.isIndependentOfAnyCollection);
    ASSERT_TRUE(constraints.requiredPosition == StageConstraints::PositionRequirement::kFirst);
    ASSERT_FALSE(constraints.requiresInputDocSource);
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, ReturnsOneDocumentPerCacheEntry) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    auto& cache = JoinPlanCache::get(getExpCtx()->getOperationContext()->getServiceContext());
    cache.put("keyA", makeCacheEntry());
    cache.put("keyB", makeCacheEntry());

    // The queue is deferred, so it is only populated once execution begins.
    auto stage = exec::agg::buildStage(DocumentSourceJoinPlanCacheStats::createFromBson(
        kEmptySpecObj.firstElement(), getExpCtx()));

    std::vector<Document> results;
    for (auto next = stage->getNext(); next.isAdvanced(); next = stage->getNext()) {
        results.push_back(next.releaseDocument());
    }
    ASSERT_EQ(2u, results.size());
    for (const auto& doc : results) {
        ASSERT_FALSE(doc.getField("planCacheKey").missing());
        // Only entries returned to a router are tagged with a shard name.
        ASSERT_TRUE(doc.getField("shard").missing());
    }
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(DocumentSourceJoinPlanCacheStatsTestCacheEnabled, TagsEntriesWithShardNameWhenFromRouter) {
    getExpCtx()->setNamespaceString(adminCollectionlessNss());
    getExpCtx()->setFromRouter(true);
    getExpCtx()->setMongoProcessInterface(std::make_shared<ShardMongoProcessInterface>());
    JoinPlanCache::get(getExpCtx()->getOperationContext()->getServiceContext())
        .put("keyA", makeCacheEntry());

    auto stage = exec::agg::buildStage(DocumentSourceJoinPlanCacheStats::createFromBson(
        kEmptySpecObj.firstElement(), getExpCtx()));

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_VALUE_EQ(Value{std::string{kShardName}}, next.getDocument().getField("shard"));
    ASSERT_TRUE(stage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo

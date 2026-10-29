// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/agg_key.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape_hash.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

namespace {

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

static constexpr auto collectionType = query_shape::CollectionType::kCollection;

class AggKeyTest : public ServiceContextTest {
public:
    static std::unique_ptr<const Key> makeAggKeyFromRawPipeline(
        const std::vector<BSONObj>& rawPipeline) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
        AggregateCommandRequest acr(kDefaultTestNss.nss());
        acr.setPipeline(rawPipeline);
        auto pipeline =
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);

        auto aggShape = std::make_unique<query_shape::AggCmdShape>(
            acr, kDefaultTestNss.nss(), pipeline->getInvolvedCollections(), *pipeline, expCtx);
        return std::make_unique<AggKey>(
            expCtx, acr, std::move(aggShape), pipeline->getInvolvedCollections(), collectionType);
    }

    size_t namespaceSize(stdx::unordered_set<NamespaceString> involvedNamespaces) {
        return std::accumulate(involvedNamespaces.begin(),
                               involvedNamespaces.end(),
                               0,
                               [](int64_t total, const auto& nss) { return total + nss.size(); });
    }
};

TEST_F(AggKeyTest, SizeOfAggCmdComponents) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    AggregateCommandRequest acr(kDefaultTestNss.nss());
    acr.setPipeline(rawPipeline);
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto namespaces = pipeline->getInvolvedCollections();
    auto aggComponents = std::make_unique<AggCmdComponents>(acr, namespaces, expCtx->getExplain());

    const auto minimumSize = sizeof(SpecificKeyComponents) +
        sizeof(stdx::unordered_set<NamespaceString>) +
        3 /*size for the two bools (_bypassDocumentValidation, _allowPartialResults) and HasField*/
        + sizeof(boost::optional<mongo::ExplainOptions::Verbosity>) + namespaceSize(namespaces);
    ASSERT_GTE(aggComponents->size(), minimumSize);
    ASSERT_LTE(aggComponents->size(), minimumSize + 8 /*padding*/);
}

TEST_F(AggKeyTest, EquivalentAggCmdComponentSizes) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    expCtx->setExplain(ExplainOptions::Verbosity::kQueryPlanner);
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};


    AggregateCommandRequest acrAllValues(kDefaultTestNss.nss());
    acrAllValues.setPipeline(rawPipeline);
    // Set all possible items in '_hasField'. None of these should affect the size.
    SimpleCursorOptions cursor;
    cursor.setBatchSize(10);
    acrAllValues.setCursor(cursor);
    acrAllValues.setExplain(true);
    acrAllValues.setBypassDocumentValidation(true);
    acrAllValues.setPassthroughToShard(PassthroughToShardOptions("shard1"));
    acrAllValues.setAllowPartialResults(true);

    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto namespaces = pipeline->getInvolvedCollections();
    auto aggComponentsAllValues =
        std::make_unique<AggCmdComponents>(acrAllValues, namespaces, expCtx->getExplain());

    // Confirm all values are set.
    BSONObjBuilder bob;
    aggComponentsAllValues->appendTo(
        bob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            R"({ bypassDocumentValidation: true, allowPartialResults: true, cursor: { batchSize: 1 }, explain: "queryPlanner", $_passthroughToShard: { shard: "?" } })"),
        bob.obj());

    // Create a request that has no values set.
    AggregateCommandRequest acrNoSetValues(kDefaultTestNss.nss());
    acrNoSetValues.setPipeline(rawPipeline);
    auto aggComponentsNoValues =
        std::make_unique<AggCmdComponents>(acrNoSetValues, namespaces, expCtx->getExplain());

    ASSERT_EQ(aggComponentsAllValues->size(), aggComponentsNoValues->size());

    // Confirm that when no optional values are set, none of them appear in the appended BSON.
    // Unlike some other commands, agg's tri-state fields have no IDL default and are simply omitted
    // when unset.
    BSONObjBuilder noValuesBob;
    aggComponentsNoValues->appendTo(
        noValuesBob, query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(BSONObj(), noValuesBob.obj());
}

TEST_F(AggKeyTest, DifferentAggCmdComponentSizes) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    AggregateCommandRequest acr(kDefaultTestNss.nss());
    acr.setPipeline(rawPipeline);
    // Manually creating different namespaces for testing purposes.
    const auto namespaceStringOne =
        NamespaceString::createNamespaceString_forTest("testDB.testColl1");
    const auto namespaceStringTwo =
        NamespaceString::createNamespaceString_forTest("testDB.testColl2");

    stdx::unordered_set<NamespaceString> smallNamespaces;
    smallNamespaces.insert(namespaceStringOne);

    stdx::unordered_set<NamespaceString> largeNamespaces;
    largeNamespaces.insert(namespaceStringOne);
    largeNamespaces.insert(namespaceStringTwo);

    auto smallAggComponents =
        std::make_unique<AggCmdComponents>(acr, smallNamespaces, expCtx->getExplain());
    auto largeAggComponents =
        std::make_unique<AggCmdComponents>(acr, largeNamespaces, expCtx->getExplain());

    ASSERT_LT(namespaceSize(smallNamespaces), namespaceSize(largeNamespaces));
    ASSERT_LT(smallAggComponents->size(), largeAggComponents->size());
}

// Testing item in opCtx that should impact key size.
TEST_F(AggKeyTest, SizeOfAggKeyWithAndWithoutComment) {
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    auto keyWithoutComment = makeAggKeyFromRawPipeline(rawPipeline);

    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    AggregateCommandRequest acrWithComment(kDefaultTestNss.nss());
    acrWithComment.setPipeline(rawPipeline);
    expCtx->getOperationContext()->setComment(BSON("comment" << " foo"));
    auto pipelineWithComment =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto aggShape =
        std::make_unique<query_shape::AggCmdShape>(acrWithComment,
                                                   kDefaultTestNss.nss(),
                                                   pipelineWithComment->getInvolvedCollections(),
                                                   *pipelineWithComment,
                                                   expCtx);
    auto keyWithComment = std::make_unique<AggKey>(expCtx,
                                                   acrWithComment,
                                                   std::move(aggShape),
                                                   pipelineWithComment->getInvolvedCollections(),
                                                   collectionType);

    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

// Testing item in command request that should impact key size.
TEST_F(AggKeyTest, SizeOfAggKeyWithAndWithoutReadConcern) {
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    auto keyWithoutReadConcern = makeAggKeyFromRawPipeline(rawPipeline);

    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    AggregateCommandRequest acrWithReadConcern(kDefaultTestNss.nss());
    acrWithReadConcern.setPipeline(rawPipeline);
    acrWithReadConcern.setReadConcern(repl::ReadConcernArgs::kLocal);
    auto pipelineWithReadConcern =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto aggShape = std::make_unique<query_shape::AggCmdShape>(
        acrWithReadConcern,
        kDefaultTestNss.nss(),
        pipelineWithReadConcern->getInvolvedCollections(),
        *pipelineWithReadConcern,
        expCtx);
    auto keyWithReadConcern =
        std::make_unique<AggKey>(expCtx,
                                 acrWithReadConcern,
                                 std::move(aggShape),
                                 pipelineWithReadConcern->getInvolvedCollections(),
                                 collectionType);

    ASSERT_LT(keyWithoutReadConcern->size(), keyWithReadConcern->size());
}

TEST_F(AggKeyTest, OriginalQueryShapeHashAppearsInKey) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto rawPipeline = {fromjson(R"({ $match: { x: 1 } })")};
    AggregateCommandRequest acr(kDefaultTestNss.nss());
    acr.setPipeline(rawPipeline);
    const auto hash = query_shape::QueryShapeHash::fromHexString(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    acr.setOriginalQueryShapeHash(hash);

    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto aggShape = std::make_unique<query_shape::AggCmdShape>(
        acr, kDefaultTestNss.nss(), pipeline->getInvolvedCollections(), *pipeline, expCtx);
    auto key = std::make_unique<AggKey>(
        expCtx, acr, std::move(aggShape), pipeline->getInvolvedCollections(), collectionType);

    const auto keyBson = key->toBson(
        expCtx->getOperationContext(),
        query_shape::SerializationOptions(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions),
        {});
    ASSERT_EQ(keyBson["originalQueryShapeHash"].str(),
              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
}

TEST_F(AggKeyTest, DifferentOriginalQueryShapeHashesProduceDifferentKeys) {
    auto makeKeyWithHash = [](std::string_view hexHash) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
        auto rawPipeline = {fromjson(R"({ $match: { x: 1 } })")};
        AggregateCommandRequest acr(kDefaultTestNss.nss());
        acr.setPipeline(rawPipeline);
        acr.setOriginalQueryShapeHash(query_shape::QueryShapeHash::fromHexString(hexHash));
        auto pipeline =
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
        auto aggShape = std::make_unique<query_shape::AggCmdShape>(
            acr, kDefaultTestNss.nss(), pipeline->getInvolvedCollections(), *pipeline, expCtx);
        return std::make_unique<AggKey>(
            expCtx, acr, std::move(aggShape), pipeline->getInvolvedCollections(), collectionType);
    };

    auto keyA = makeKeyWithHash("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auto keyB = makeKeyWithHash("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    ASSERT_NE(absl::HashOf(*keyA), absl::HashOf(*keyB));
}

TEST_F(AggKeyTest, AllowPartialResultsTriState) {
    auto makeKey = [](boost::optional<bool> allowPartialResults) {
        auto nss = kDefaultTestNss.nss();
        auto expCtx = make_intrusive<ExpressionContextForTest>(nss);
        auto rawPipeline = {fromjson(R"({ $match: { x: 1 } })")};
        AggregateCommandRequest acr(kDefaultTestNss.nss());
        acr.setPipeline(rawPipeline);
        if (allowPartialResults) {
            acr.setAllowPartialResults(*allowPartialResults);
        }
        auto pipeline =
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
        auto aggShape = std::make_unique<query_shape::AggCmdShape>(
            acr, nss, pipeline->getInvolvedCollections(), *pipeline, expCtx);
        return std::make_unique<AggKey>(
            expCtx, acr, std::move(aggShape), pipeline->getInvolvedCollections(), collectionType);
    };

    auto keyOmitted = makeKey(boost::none);
    auto keyTrue = makeKey(true);
    auto keyFalse = makeKey(false);

    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    const auto opts = query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions;

    auto omittedBson = keyOmitted->toBson(expCtx->getOperationContext(), opts, {});
    ASSERT_FALSE(omittedBson.hasField("allowPartialResults"));

    auto trueBson = keyTrue->toBson(expCtx->getOperationContext(), opts, {});
    ASSERT_TRUE(trueBson.hasField("allowPartialResults"));
    ASSERT_TRUE(trueBson["allowPartialResults"].Bool());

    auto falseBson = keyFalse->toBson(expCtx->getOperationContext(), opts, {});
    ASSERT_TRUE(falseBson.hasField("allowPartialResults"));
    ASSERT_FALSE(falseBson["allowPartialResults"].Bool());

    ASSERT_NE(absl::HashOf(*keyOmitted), absl::HashOf(*keyTrue));
    ASSERT_NE(absl::HashOf(*keyOmitted), absl::HashOf(*keyFalse));
    ASSERT_NE(absl::HashOf(*keyTrue), absl::HashOf(*keyFalse));
}

}  // namespace
}  // namespace mongo::query_stats

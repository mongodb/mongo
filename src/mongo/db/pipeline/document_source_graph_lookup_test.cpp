/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_graph_lookup.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <deque>
#include <initializer_list>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGraphLookUpTest = AggregationContextFixture;

/**
 * This fixture also provides storage engine for spilling.
 */
class DocumentSourceGraphLookUpSpillingTest : public DocumentSourceGraphLookUpTest {
public:
    DocumentSourceGraphLookUpSpillingTest()
        : AggregationContextFixture(std::make_unique<MongoDScopedGlobalServiceContextForTest>(
              MongoDScopedGlobalServiceContextForTest::Options{}, shouldSetupTL)) {}
};

//
// Evaluation.
//

/**
 * A MongoProcessInterface use for testing that supports making pipelines with an initial
 * DocumentSourceMock source.
 */
class MockMongoInterface final : public StandaloneProcessInterface {
public:
    MockMongoInterface(std::deque<DocumentSource::GetNextResult> results)
        : StandaloneProcessInterface(nullptr), _results(std::move(results)) {}

    std::unique_ptr<Pipeline> preparePipelineForExecution(
        Pipeline* ownedPipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final {
        std::unique_ptr<Pipeline> pipeline(ownedPipeline);
        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_results, pipeline->getContext()));
        return pipeline;
    }

    std::unique_ptr<mongo::Pipeline> preparePipelineForExecution(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none,
        bool shouldUseCollectionDefaultCollator = false) final {
        return preparePipelineForExecution(pipeline, shardTargetingPolicy, readConcern);
    }

private:
    std::deque<DocumentSource::GetNextResult> _results;
};

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenDoingInitialMatchIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();
    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = exec::agg::MockStage::createForTest(std::move(inputs), expCtx);

    std::deque<DocumentSource::GetNextResult> fromContents{Document{{"to", 0}}};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "graph_lookup");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "_id"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);
    graphLookupStage->setSource(inputMock.get());
    ASSERT_THROWS_CODE(graphLookupStage->getNext(), AssertionException, 40271);
}

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenExploringGraphIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = exec::agg::MockStage::createForTest(std::move(inputs), expCtx);

    std::deque<DocumentSource::GetNextResult> fromContents{
        Document{{"_id", "a"_sd}, {"to", 0}, {"from", 1}}, Document{{"to", 1}}};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "graph_lookup");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "_id"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);
    graphLookupStage->setSource(inputMock.get());

    ASSERT_THROWS_CODE(graphLookupStage->getNext(), AssertionException, 40271);
}

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldErrorWhenHandlingUnwindIfDocumentInFromCollectionIsMissingId) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = exec::agg::MockStage::createForTest(std::move(inputs), expCtx);

    std::deque<DocumentSource::GetNextResult> fromContents{Document{{"to", 0}}};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "graph_lookup");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto unwindStage = DocumentSourceUnwind::create(expCtx, "results", false, boost::none);
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "_id"),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);
    graphLookupStage->setSource(inputMock.get());

    ASSERT_THROWS_CODE(graphLookupStage->getNext(), AssertionException, 40271);
}

bool arrayContains(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   const std::vector<Value>& arr,
                   const Value& elem) {
    auto result = std::find_if(arr.begin(), arr.end(), [&expCtx, &elem](const Value& other) {
        return expCtx->getValueComparator().evaluate(elem == other);
    });
    return result != arr.end();
}

TEST_F(DocumentSourceGraphLookUpTest,
       ShouldTraverseSubgraphIfIdOfDocumentsInFromCollectionAreNonUnique) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}}};
    auto inputMock = exec::agg::MockStage::createForTest(std::move(inputs), expCtx);

    Document to0from1{{"_id", "a"_sd}, {"to", 0}, {"from", 1}};
    Document to0from2{{"_id", "a"_sd}, {"to", 0}, {"from", 2}};
    Document to1{{"_id", "b"_sd}, {"to", 1}};
    Document to2{{"_id", "c"_sd}, {"to", 2}};
    std::deque<DocumentSource::GetNextResult> fromContents{
        Document(to1), Document(to2), Document(to0from1), Document(to0from2)};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "graph_lookup");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "_id"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);
    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    ASSERT_EQ(2ULL, next.getDocument().computeSize());
    ASSERT_VALUE_EQ(Value(0), next.getDocument().getField("_id"));

    auto resultsValue = next.getDocument().getField("results");
    ASSERT(resultsValue.isArray());
    auto resultsArray = resultsValue.getArray();

    // Since 'to0from1' and 'to0from2' have the same _id, we should end up only exploring the path
    // through one of them.
    if (arrayContains(expCtx, resultsArray, Value(to0from1))) {
        // If 'to0from1' was returned, then we should see 'to1' and nothing else.
        ASSERT(arrayContains(expCtx, resultsArray, Value(to1)));
        ASSERT_EQ(2U, resultsArray.size());

        next = graphLookupStage->getNext();
        ASSERT(next.isEOF());
    } else if (arrayContains(expCtx, resultsArray, Value(to0from2))) {
        // If 'to0from2' was returned, then we should see 'to2' and nothing else.
        ASSERT(arrayContains(expCtx, resultsArray, Value(to2)));
        ASSERT_EQ(2U, resultsArray.size());

        next = graphLookupStage->getNext();
        ASSERT(next.isEOF());
    } else {
        FAIL(std::string(str::stream() << "Expected either [ " << to0from1.toString() << " ] or [ "
                                       << to0from2.toString() << " ] but found [ "
                                       << next.getDocument().toString() << " ]"));
    }
}

TEST_F(DocumentSourceGraphLookUpTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();

    auto inputMock =
        exec::agg::MockStage::createForTest({Document{{"startPoint", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"startPoint", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            expCtx);

    std::deque<DocumentSource::GetNextResult> fromContents{
        Document{{"_id", "a"_sd}, {"to", 0}, {"from", 1}}, Document{{"_id", "b"_sd}, {"to", 1}}};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // We expect {startPoint: 0, results: [{_id: "a", to: 0, from: 1}, {_id: "b", to: 1}]}, but the
    // 'results' array can be in any order. So we use arrayContains to assert it has the right
    // contents.
    auto result = next.releaseDocument();
    ASSERT_VALUE_EQ(result["startPoint"], Value(0));
    ASSERT_EQ(result["results"].getType(), BSONType::array);
    ASSERT_EQ(result["results"].getArray().size(), 2UL);
    ASSERT_TRUE(arrayContains(expCtx,
                              result["results"].getArray(),
                              Value(Document{{"_id", "a"_sd}, {"to", 0}, {"from", 1}})));
    ASSERT_TRUE(arrayContains(
        expCtx, result["results"].getArray(), Value(Document{{"_id", "b"_sd}, {"to", 1}})));

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    result = next.releaseDocument();
    ASSERT_VALUE_EQ(result["startPoint"], Value(0));
    ASSERT_EQ(result["results"].getType(), BSONType::array);
    ASSERT_EQ(result["results"].getArray().size(), 2UL);
    ASSERT_TRUE(arrayContains(expCtx,
                              result["results"].getArray(),
                              Value(Document{{"_id", "a"_sd}, {"to", 0}, {"from", 1}})));
    ASSERT_TRUE(arrayContains(
        expCtx, result["results"].getArray(), Value(Document{{"_id", "b"_sd}, {"to", 1}})));

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
}

TEST_F(DocumentSourceGraphLookUpTest, ShouldPropagatePausesWhileUnwinding) {
    auto expCtx = getExpCtx();

    // Set up the $graphLookup stage
    auto inputMock =
        exec::agg::MockStage::createForTest({Document{{"startPoint", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"startPoint", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            expCtx);

    std::deque<DocumentSource::GetNextResult> fromContents{
        Document{{"_id", "a"_sd}, {"to", 0}, {"from", 1}}, Document{{"_id", "b"_sd}, {"to", 1}}};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");

    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    auto unwindStage = DocumentSourceUnwind::create(
        expCtx, "results", preserveNullAndEmptyArrays, includeArrayIndex);

    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    // Assert it has the expected results. Note the results can be in either order.
    auto expectedA =
        Document{{"startPoint", 0}, {"results", Document{{"_id", "a"_sd}, {"to", 0}, {"from", 1}}}};
    auto expectedB = Document{{"startPoint", 0}, {"results", Document{{"_id", "b"_sd}, {"to", 1}}}};
    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    if (expCtx->getDocumentComparator().evaluate(next.getDocument() == expectedA)) {
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
    } else {
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedA);
    }

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    if (expCtx->getDocumentComparator().evaluate(next.getDocument() == expectedA)) {
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
    } else {
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedB);
        next = graphLookupStage->getNext();
        ASSERT_TRUE(next.isAdvanced());
        ASSERT_DOCUMENT_EQ(next.releaseDocument(), expectedA);
    }

    ASSERT_TRUE(graphLookupStage->getNext().isPaused());

    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
}

TEST_F(DocumentSourceGraphLookUpSpillingTest, ShouldSpillVisitedDocuments) {
    static constexpr long long kMemoryLimit = 100 * 1024;
    RAIIServerParameterControllerForTest memoryLimitController(
        "internalDocumentSourceGraphLookupMaxMemoryBytes", kMemoryLimit);

    auto expCtx = getExpCtx();
    auto inputMock = exec::agg::MockStage::createForTest({Document{{"startPoint", 0}}}, expCtx);

    static constexpr long long kPaddingSize = 1024;
    std::string padding(kPaddingSize, 'a');
    std::deque<DocumentSource::GetNextResult> fromContents;
    for (long long i = 0; i < kMemoryLimit / kPaddingSize; ++i) {
        fromContents.push_back(
            Document{{{"_id", Value{i}}, {"to", 0}, {"from", 1}, {"padding", padding}}});
    }

    std::vector<Value> expectedResults;
    expectedResults.reserve(fromContents.size());
    for (const auto& doc : fromContents) {
        expectedResults.emplace_back(doc.getDocument());
    }
    std::sort(
        expectedResults.begin(), expectedResults.end(), expCtx->getValueComparator().getLessThan());

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(fromContents));
    expCtx->setAllowDiskUse(true);

    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    std::vector<Value> results = next.getDocument().getField("results").getArray();
    std::sort(results.begin(), results.end(), expCtx->getValueComparator().getLessThan());

    ASSERT_VALUE_EQ(Value{expectedResults}, Value{results});
    ASSERT_TRUE(graphLookupStage->getNext().isEOF());

    ASSERT_TRUE(graphLookupStage->usedDisk());
    auto stats =
        dynamic_cast<const DocumentSourceGraphLookupStats*>(graphLookupStage->getSpecificStats())
            ->spillingStats;
    ASSERT_GTE(stats.getSpills(), 1);
    ASSERT_GTE(stats.getSpilledRecords(), 70);
}

TEST_F(DocumentSourceGraphLookUpSpillingTest, ShouldSpillSeveralStructures) {
    static constexpr long long kMemoryLimit = 200 * 1024;
    RAIIServerParameterControllerForTest memoryLimitController(
        "internalDocumentSourceGraphLookupMaxMemoryBytes", kMemoryLimit);

    auto expCtx = getExpCtx();
    auto inputMock = exec::agg::MockStage::createForTest({Document{{"startPoint", 0}}}, expCtx);

    static constexpr long long kPaddingSize = 1024;
    std::string padding(kPaddingSize, 'a');
    std::deque<DocumentSource::GetNextResult> fromContents;
    // All documents will fit into memory limit by themselves, but in total with from values set and
    // queue memory should overflow.
    for (long long i = 0; i < 100; ++i) {
        std::string currentPadding = padding + std::to_string(i);
        fromContents.push_back(Document{{{"_id", Value{i}}, {"to", 0}, {"from", currentPadding}}});
    }

    std::vector<Value> expectedResults;
    expectedResults.reserve(fromContents.size());
    for (const auto& doc : fromContents) {
        expectedResults.emplace_back(doc.getDocument());
    }
    std::sort(
        expectedResults.begin(), expectedResults.end(), expCtx->getValueComparator().getLessThan());

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(fromContents));
    expCtx->setAllowDiskUse(true);

    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    std::vector<Value> results = next.getDocument().getField("results").getArray();
    std::sort(results.begin(), results.end(), expCtx->getValueComparator().getLessThan());

    ASSERT_VALUE_EQ(Value{expectedResults}, Value{results});
    ASSERT_TRUE(graphLookupStage->getNext().isEOF());

    ASSERT_TRUE(graphLookupStage->usedDisk());
    auto stats =
        dynamic_cast<const DocumentSourceGraphLookupStats*>(graphLookupStage->getSpecificStats())
            ->spillingStats;
    ASSERT_GTE(stats.getSpills(), 2);
    ASSERT_GTE(stats.getSpilledRecords(), 70);
}

TEST_F(DocumentSourceGraphLookUpSpillingTest, CanForceSpill) {
    auto expCtx = getExpCtx();
    auto inputMock = exec::agg::MockStage::createForTest({Document{{"startPoint", 0}}}, expCtx);

    static constexpr size_t kResultCount = 100;
    std::deque<DocumentSource::GetNextResult> fromContents;
    for (size_t i = 0; i < kResultCount; ++i) {
        fromContents.push_back(
            Document{{{"_id", Value{static_cast<long long>(i)}}, {"to", 0}, {"from", 1}}});
    }

    std::vector<Value> expectedResults;
    expectedResults.reserve(fromContents.size());
    for (const auto& doc : fromContents) {
        expectedResults.emplace_back(doc.getDocument());
    }
    std::sort(
        expectedResults.begin(), expectedResults.end(), expCtx->getValueComparator().getLessThan());

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(fromContents));
    expCtx->setAllowDiskUse(true);

    auto unwindStage = DocumentSourceUnwind::create(expCtx, "results", false, boost::none);
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    std::vector<Value> results;
    results.reserve(kResultCount);

    for (size_t i = 0; i < kResultCount / 10; ++i) {
        for (size_t j = 0; j < 10; ++j) {
            results.emplace_back(graphLookupStage->getNext().getDocument().getField("results"));
        }
        graphLookupStage->forceSpill();
    }

    std::sort(results.begin(), results.end(), expCtx->getValueComparator().getLessThan());
    ASSERT_EQ(results.size(), expectedResults.size());
    for (size_t i = 0; i < results.size(); ++i) {
        ASSERT_VALUE_EQ(results[i], expectedResults[i]);
    }

    ASSERT_TRUE(graphLookupStage->getNext().isEOF());
    ASSERT_TRUE(graphLookupStage->usedDisk());
    auto stats =
        dynamic_cast<const DocumentSourceGraphLookupStats*>(graphLookupStage->getSpecificStats())
            ->spillingStats;

    // There is only one actual spill the first time we call doForceSpill(). Following calls just
    // remove buffered documents, but don't write anything to disk.
    ASSERT_EQ(stats.getSpills(), 1);
    ASSERT_EQ(stats.getSpilledRecords(), 90);
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupShouldReportAsFieldIsModified) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));
    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);

    auto modifiedPaths = graphLookupStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(1U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("results"));
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupShouldReportFieldsModifiedByAbsorbedUnwind) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{}));
    auto unwindStage =
        DocumentSourceUnwind::create(expCtx, "results", false, std::string("arrIndex"));
    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);

    auto modifiedPaths = graphLookupStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(2U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("results"));
    ASSERT_EQ(1U, modifiedPaths.paths.count("arrIndex"));
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupWithComparisonExpressionForStartWith) {
    auto expCtx = getExpCtx();

    auto inputMock =
        exec::agg::MockStage::createForTest(Document({{"_id", 0}, {"a", 1}, {"b", 2}}), expCtx);

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    std::deque<DocumentSource::GetNextResult> fromContents{Document{{"_id", 0}, {"to", true}},
                                                           Document{{"_id", 1}, {"to", false}}};
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));

    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionCompare::create(expCtx.get(),
                                  ExpressionCompare::GT,
                                  ExpressionFieldPath::deprecatedCreate(expCtx.get(), "a"),
                                  ExpressionFieldPath::deprecatedCreate(expCtx.get(), "b")),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    // The 'startWith' expression evaluates to false, so we expect the 'results' array to contain
    // just one document.
    auto actualResult = next.releaseDocument();
    Document expectedResult{
        {"_id", 0},
        {"a", 1},
        {"b", 2},
        {"results", std::vector<Value>{Value{Document{{"_id", 1}, {"to", false}}}}}};
    ASSERT_DOCUMENT_EQ(actualResult, expectedResult);
}

TEST_F(DocumentSourceGraphLookUpTest, ShouldExpandArraysAtEndOfConnectFromField) {
    auto expCtx = getExpCtx();

    std::deque<DocumentSource::GetNextResult> inputs{Document{{"_id", 0}, {"startVal", 0}}};
    auto inputMock = exec::agg::MockStage::createForTest(std::move(inputs), expCtx);

    /* Make the following graph:
     *   ,> 1 .
     *  /      \
     * 0 -> 2 --+-> 4
     *  \      /
     *   `> 3 '
     */
    Document startDoc{{"_id", 0}, {"to", std::vector{1, 2, 3}}};
    Document middle1{{"_id", 1}, {"to", 4}};
    Document middle2{{"_id", 2}, {"to", 4}};
    Document middle3{{"_id", 3}, {"to", 4}};
    Document sinkDoc{{"_id", 4}};

    // GetNextResults are only constructable from an rvalue reference to a Document, so we have to
    // explicitly copy.
    std::deque<DocumentSource::GetNextResult> fromContents{Document(startDoc),
                                                           Document(middle1),
                                                           Document(middle2),
                                                           Document(middle3),
                                                           Document(sinkDoc)};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "graph_lookup");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "to",
        "_id",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startVal"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    ASSERT_EQ(3ULL, next.getDocument().computeSize());
    ASSERT_VALUE_EQ(Value(0), next.getDocument().getField("_id"));

    auto resultsValue = next.getDocument().getField("results");
    ASSERT(resultsValue.isArray());
    auto resultsArray = resultsValue.getArray();

    ASSERT(arrayContains(expCtx, resultsArray, Value(middle1)));
    ASSERT(arrayContains(expCtx, resultsArray, Value(middle2)));
    ASSERT(arrayContains(expCtx, resultsArray, Value(middle3)));
    ASSERT(arrayContains(expCtx, resultsArray, Value(sinkDoc)));
    ASSERT(graphLookupStage->getNext().isEOF());
}

TEST_F(DocumentSourceGraphLookUpTest, ShouldNotExpandArraysWithinArraysAtEndOfConnectFromField) {
    auto expCtx = getExpCtx();

    auto makeTupleValue = [](int left, int right) {
        return Value(std::vector<Value>{Value(left), Value(right)});
    };

    std::deque<DocumentSource::GetNextResult> inputs{
        Document{{"_id", 0}, {"startVal", makeTupleValue(0, 0)}}};
    auto inputMock = exec::agg::MockStage::createForTest(std::move(inputs), expCtx);

    // Make the following graph:
    //
    // [0, 0] -> [1, 1]
    //  |
    //  v
    // [2, 2]
    //
    // (unconnected)
    // [1, 2]

    // If the connectFromField were doubly expanded, we would query for connectToValues with an
    // expression like {$in: [1, 2]} instead of {$in: [[1, 1], [2, 2]]}, the former of which would
    // also include [1, 2].
    Document startDoc{
        {"_id", 0},
        {"coordinate", makeTupleValue(0, 0)},
        {"connectedTo",
         std::vector<Value>{makeTupleValue(1, 1), makeTupleValue(2, 2)}}};  // Note the extra array.
    Document target1{{"_id", 1}, {"coordinate", makeTupleValue(1, 1)}};
    Document target2{{"_id", 2}, {"coordinate", makeTupleValue(2, 2)}};
    Document soloDoc{{"_id", 3}, {"coordinate", makeTupleValue(1, 2)}};

    // GetNextResults are only constructable from an rvalue reference to a Document, so we have to
    // explicitly copy.
    std::deque<DocumentSource::GetNextResult> fromContents{
        Document(startDoc), Document(target1), Document(target2), Document(soloDoc)};

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "graph_lookup");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<MockMongoInterface>(std::move(fromContents)));
    auto graphLookupDS = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "connectedTo",
        "coordinate",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startVal"),
        boost::none,
        boost::none,
        boost::none,
        boost::none);
    auto graphLookupStage = exec::agg::buildStage(graphLookupDS);

    graphLookupStage->setSource(inputMock.get());
    graphLookupStage->setSource(inputMock.get());

    auto next = graphLookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());

    ASSERT_EQ(3ULL, next.getDocument().computeSize());
    ASSERT_VALUE_EQ(Value(0), next.getDocument().getField("_id"));

    auto resultsValue = next.getDocument().getField("results");
    ASSERT(resultsValue.isArray());
    auto resultsArray = resultsValue.getArray();

    ASSERT(arrayContains(expCtx, resultsArray, Value(target1)));
    ASSERT(arrayContains(expCtx, resultsArray, Value(target2)));
    ASSERT(!arrayContains(expCtx, resultsArray, Value(soloDoc)));
    ASSERT(graphLookupStage->getNext().isEOF());
}

TEST_F(DocumentSourceGraphLookUpTest, IncrementNestedAggregateOpCounterOnCreateButNotOnCopy) {
    auto testOpCounter = [&](const NamespaceString& nss, const int expectedIncrease) {
        auto resolvedNss = ResolvedNamespaceMap{{nss, {nss, std::vector<BSONObj>()}}};
        auto countBeforeCreate = serviceOpCounters(getOpCtx()).getNestedAggregate()->load();

        // Create a DocumentSourceGraphLookUp and verify that the counter increases by the expected
        // amount.
        auto originalExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        originalExpCtx->setResolvedNamespaces(resolvedNss);
        auto docSource = DocumentSourceGraphLookUp::createFromBson(
            BSON("$graphLookup" << BSON("from" << nss.coll() << "startWith"
                                               << "$x"
                                               << "connectFromField"
                                               << "id"
                                               << "connectToField"
                                               << "id"
                                               << "as"
                                               << "connections"))
                .firstElement(),
            originalExpCtx);
        auto originalGraphLookup = static_cast<DocumentSourceGraphLookUp*>(docSource.get());
        auto countAfterCreate = serviceOpCounters(getOpCtx()).getNestedAggregate()->load();
        ASSERT_EQ(countAfterCreate - countBeforeCreate, expectedIncrease);

        // Copy the DocumentSourceGraphLookUp and verify that the counter doesn't increase.
        auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        newExpCtx->setResolvedNamespaces(resolvedNss);
        DocumentSourceGraphLookUp newGraphLookup{*originalGraphLookup, newExpCtx};
        auto countAfterCopy = serviceOpCounters(getOpCtx()).getNestedAggregate()->load();
        ASSERT_EQ(countAfterCopy - countAfterCreate, 0);
    };

    testOpCounter(NamespaceString::createNamespaceString_forTest("testDb", "testColl"), 1);
    // $graphLookup against internal databases should not cause the counter to get incremented.
    testOpCounter(NamespaceString::createNamespaceString_forTest("config", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("admin", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("local", "testColl"), 0);
}

TEST_F(DocumentSourceGraphLookUpTest, RedactionStartWithSingleField) {
    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        getExpCtx()->getNamespaceString().dbName(), "coll"));
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        "$graphLookup": {
            "from": "coll",
            "startWith": "$a.b",
            "connectFromField": "c.d",
            "connectToField": "e.f",
            "as": "x",
            "depthField": "y",
            "maxDepth": 5,
            "restrictSearchWithMatch": {
                "foo": "abc",
                "bar.baz": { "$gt": 5 }
            }
        }
    })");
    auto docSource = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$graphLookup": {
                "from": "HASH<coll>",
                "as": "HASH<x>",
                "connectToField": "HASH<e>.HASH<f>",
                "connectFromField": "HASH<c>.HASH<d>",
                "startWith": "$HASH<a>.HASH<b>",
                "depthField": "HASH<y>",
                "maxDepth": "?number",
                "restrictSearchWithMatch": {
                    "$and": [
                        {
                            "HASH<foo>": {
                                "$eq": "?string"
                            }
                        },
                        {
                            "HASH<bar>.HASH<baz>": {
                                "$gt": "?number"
                            }
                        }
                    ]
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGraphLookUpTest, RedactionStartWithArrayOfFields) {
    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        getExpCtx()->getNamespaceString().dbName(), "coll"));
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $graphLookup: {
            from: "coll",
            startWith: ["$a.b", "$bar.baz"],
            connectFromField: "x",
            connectToField: "y",
            as: "z"
        }
    })");
    auto docSource = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$graphLookup": {
                "from": "HASH<coll>",
                "as": "HASH<z>",
                "connectToField": "HASH<y>",
                "connectFromField": "HASH<x>",
                "startWith": ["$HASH<a>.HASH<b>", "$HASH<bar>.HASH<baz>"]
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGraphLookUpTest, RedactionWithAbsorbedUnwind) {
    auto expCtx = getExpCtx();

    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll"));
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto unwindStage = DocumentSourceUnwind::create(expCtx, "results", false, boost::none);
    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        getExpCtx(),
        graphLookupNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::deprecatedCreate(expCtx.get(), "startPoint"),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);

    auto serialized = redactToArray(*graphLookupStage);
    ASSERT_EQ(2, serialized.size());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$graphLookup": {
                "from": "HASH<coll>",
                "as": "HASH<results>",
                "connectToField": "HASH<to>",
                "connectFromField": "HASH<from>",
                "startWith": "$HASH<startPoint>"
            }
        })",
        serialized[0].getDocument().toBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$unwind": {
                path: "$HASH<results>"
            }
        })",
        serialized[1].getDocument().toBson());
}

using DocumentSourceGraphLookupServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceGraphLookupServerlessTest,
       LiteParsedDocumentSourceLookupStringExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancySupportController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    auto originalBSON = BSON("$graphLookup" << BSON("from" << "foo"
                                                           << "startWith"
                                                           << "$x"
                                                           << "connectFromField"
                                                           << "id"
                                                           << "connectToField"
                                                           << "id"
                                                           << "as"
                                                           << "connections"));

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), _targetColl);
    auto liteParsedLookup = DocumentSourceGraphLookUp::LiteParsed::parse(
        nss, originalBSON.firstElement(), LiteParserOptions{});
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  expCtx->getNamespaceString().dbName(), "foo")));
}

TEST_F(DocumentSourceGraphLookupServerlessTest,
       CreateFromBSONContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    auto tenantId = expCtx->getNamespaceString().tenantId();
    ASSERT(tenantId);

    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "foo"));
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto spec = BSON("$graphLookup" << BSON("from" << "foo"
                                                   << "startWith"
                                                   << "$x"
                                                   << "connectFromField"
                                                   << "id"
                                                   << "connectToField"
                                                   << "id"
                                                   << "as"
                                                   << "connections"));
    auto graphLookupStage = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), expCtx);
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest({}, expCtx), graphLookupStage}, expCtx);
    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT_EQ(1ul, involvedNssSet.count(graphLookupNs));
}

}  // namespace
}  // namespace mongo

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

#include "mongo/db/pipeline/document_source_bucket_auto.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/bucket_auto_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#include <bitset>
#include <cmath>
#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::deque;
using std::string;
using std::vector;

class BucketAutoTests : public AggregationContextFixture {
public:
    intrusive_ptr<DocumentSource> createBucketAuto(BSONObj bucketAutoSpec) {
        return DocumentSourceBucketAuto::createFromBson(bucketAutoSpec.firstElement(), getExpCtx());
    }

    intrusive_ptr<exec::agg::BucketAutoStage> createBucketAutoStage(
        BSONObj bucketAutoSpec, const exec::agg::StagePtr& sourceStage) {
        return boost::dynamic_pointer_cast<exec::agg::BucketAutoStage>(
            exec::agg::buildStageAndStitch(createBucketAuto(bucketAutoSpec), sourceStage));
    }

    intrusive_ptr<exec::agg::BucketAutoStage> createBucketAutoStage(
        const intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& groupByExpression,
        int numBuckets,
        const exec::agg::StagePtr& sourceStage) {
        return boost::dynamic_pointer_cast<exec::agg::BucketAutoStage>(
            exec::agg::buildStageAndStitch(
                DocumentSourceBucketAuto::create(expCtx, groupByExpression, numBuckets),
                sourceStage));
    }

    vector<Document> getResults(BSONObj bucketAutoSpec,
                                deque<Document> inputs,
                                bool expectedMemUse = true) {

        // Convert Documents to GetNextResults.
        deque<DocumentSource::GetNextResult> mockInputs;
        for (auto&& input : inputs) {
            mockInputs.emplace_back(std::move(input));
        }

        auto mockStage = exec::agg::MockStage::createForTest(std::move(mockInputs), getExpCtx());
        auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

        vector<Document> results;
        for (auto next = bucketAutoStage->getNext(); next.isAdvanced();
             next = bucketAutoStage->getNext()) {
            results.push_back(next.releaseDocument());
        }

        if (expectedMemUse) {
            ASSERT_GT(bucketAutoStage->getMemoryTracker_forTest()->peakTrackedMemoryBytes(), 0);
        }

        return results;
    }

    void testSerialize(BSONObj bucketAutoSpec, BSONObj expectedObj) {
        auto bucketAutoStage = createBucketAuto(bucketAutoSpec);
        assertBucketAutoType(bucketAutoStage);

        vector<Value> explainedStages;
        bucketAutoStage->serializeToArray(
            explainedStages,
            SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)});
        ASSERT_EQUALS(explainedStages.size(), 1UL);

        Value expectedExplain = Value(expectedObj);

        auto bucketAutoExplain = explainedStages[0];
        ASSERT_VALUE_EQ(bucketAutoExplain["$bucketAuto"], expectedExplain);
    }

private:
    void assertBucketAutoType(intrusive_ptr<DocumentSource> documentSource) {
        const auto* bucketAutoStage = dynamic_cast<DocumentSourceBucketAuto*>(documentSource.get());
        ASSERT(bucketAutoStage);
    }
    const DocumentSourceBucketAuto* getBucketAutoPtr(
        intrusive_ptr<exec::agg::Stage> documentSource) {
        const auto* bucketAutoStage = dynamic_cast<DocumentSourceBucketAuto*>(documentSource.get());
        ASSERT(bucketAutoStage);
        return bucketAutoStage;
    }
};

TEST_F(BucketAutoTests, ReturnsNoBucketsWhenSourceIsEmpty) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets: 1}}");
    auto results =
        getResults(bucketAutoSpec /*bucketAutoSpec*/, {} /*inputs*/, false /*expectedMemUse*/);
    ASSERT_EQUALS(results.size(), 0UL);
}

TEST_F(BucketAutoTests, Returns1Of1RequestedBucketWhenAllUniqueValues) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets: 1}}");

    // Values are 1, 2, 3, 4
    auto results = getResults(
        bucketAutoSpec,
        {Document{{"x", 4}}, Document{{"x", 1}}, Document{{"x", 3}}, Document{{"x", 2}}});
    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 1, max : 4}, count : 4}")));

    // Values are 'a', 'b', 'c', 'd'
    results = getResults(bucketAutoSpec,
                         {Document{{"x", "d"_sd}},
                          Document{{"x", "b"_sd}},
                          Document{{"x", "a"_sd}},
                          Document{{"x", "c"_sd}}});
    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 'a', max : 'd'}, count : 4}")));
}

TEST_F(BucketAutoTests, Returns1Of1RequestedBucketWithNonUniqueValues) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets: 1}}");

    // Values are 1, 2, 7, 7, 7
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 7}},
                               Document{{"x", 1}},
                               Document{{"x", 7}},
                               Document{{"x", 2}},
                               Document{{"x", 7}}});
    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 1, max : 7}, count : 5}")));
}

TEST_F(BucketAutoTests, Returns1Of1RequestedBucketWhen1ValueInSource) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets: 1}}");
    auto results = getResults(bucketAutoSpec, {Document{{"x", 1}}});
    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 1, max : 1}, count : 1}")));

    results = getResults(bucketAutoSpec, {Document{{"x", "a"_sd}}});
    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 'a', max : 'a'}, count : 1}")));
}

TEST_F(BucketAutoTests, Returns2Of2RequestedBucketsWhenSmallestValueHasManyDuplicates) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}");

    // Values are 1, 1, 1, 1, 2
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 1}},
                               Document{{"x", 1}},
                               Document{{"x", 1}},
                               Document{{"x", 2}},
                               Document{{"x", 1}}});
    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 1, max : 2}, count : 4}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 2, max : 2}, count : 1}")));
}

TEST_F(BucketAutoTests, Returns2Of2RequestedBucketsWhenLargestValueHasManyDuplicates) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}");

    // Values are 0, 1, 2, 3, 4, 5, 5, 5, 5
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 5}},
                               Document{{"x", 0}},
                               Document{{"x", 2}},
                               Document{{"x", 3}},
                               Document{{"x", 5}},
                               Document{{"x", 1}},
                               Document{{"x", 5}},
                               Document{{"x", 4}},
                               Document{{"x", 5}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 5}, count : 5}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 5, max : 5}, count : 4}")));
}

TEST_F(BucketAutoTests, Returns3Of3RequestedBucketsWhenAllUniqueValues) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 3}}");

    // Values are 0, 1, 2, 3, 4, 5, 6, 7
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 2}},
                               Document{{"x", 4}},
                               Document{{"x", 1}},
                               Document{{"x", 7}},
                               Document{{"x", 0}},
                               Document{{"x", 5}},
                               Document{{"x", 3}},
                               Document{{"x", 6}}});

    ASSERT_EQUALS(results.size(), 3UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 3}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 3, max : 6}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[2], Document(fromjson("{_id : {min : 6, max : 7}, count : 2}")));
}

TEST_F(BucketAutoTests, Returns2Of3RequestedBucketsWhenLargestValueHasManyDuplicates) {
    // In this case, two buckets will be made because the approximate bucket size calculated will be
    // 7/3, which rounds to 2. Therefore, the boundaries will be calculated so that values 0 and 1
    // into the first bucket. All of the 2 values will then fall into a second bucket.
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 3}}");

    // Values are 0, 1, 2, 2, 2, 2, 2
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 2}},
                               Document{{"x", 0}},
                               Document{{"x", 2}},
                               Document{{"x", 2}},
                               Document{{"x", 1}},
                               Document{{"x", 2}},
                               Document{{"x", 2}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 2}, count : 2}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 2, max : 2}, count : 5}")));
}

TEST_F(BucketAutoTests, Returns1Of3RequestedBucketsWhenLargestValueHasManyDuplicates) {
    // In this case, one bucket will be made because the approximate bucket size calculated will be
    // 8/3, which rounds to 3. Therefore, the boundaries will be calculated so that values 0, 1, and
    // 2 fall into the first bucket. Since 2 is repeated many times, all of the 2 values will be
    // pulled into the first bucket.
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 3}}");

    // Values are 0, 1, 2, 2, 2, 2, 2, 2
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 2}},
                               Document{{"x", 2}},
                               Document{{"x", 0}},
                               Document{{"x", 2}},
                               Document{{"x", 2}},
                               Document{{"x", 2}},
                               Document{{"x", 1}},
                               Document{{"x", 2}}});

    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 2}, count : 8}")));
}

TEST_F(BucketAutoTests, Returns3Of3RequestedBucketsWhen3ValuesInSource) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 3}}");
    auto results =
        getResults(bucketAutoSpec, {Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 2}}});

    ASSERT_EQUALS(results.size(), 3UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 1}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 1, max : 2}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[2], Document(fromjson("{_id : {min : 2, max : 2}, count : 1}")));
}

TEST_F(BucketAutoTests, Returns3Of10RequestedBucketsWhen3ValuesInSource) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 10}}");
    auto results =
        getResults(bucketAutoSpec, {Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 2}}});

    ASSERT_EQUALS(results.size(), 3UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 1}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 1, max : 2}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[2], Document(fromjson("{_id : {min : 2, max : 2}, count : 1}")));
}

TEST_F(BucketAutoTests, PopulatesLastBucketWithRemainingDocuments) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 5}}");
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 0}},
                               Document{{"x", 1}},
                               Document{{"x", 2}},
                               Document{{"x", 3}},
                               Document{{"x", 4}},
                               Document{{"x", 5}},
                               Document{{"x", 6}}});
    ASSERT_EQUALS(results.size(), 5UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 1}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 1, max : 2}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[2], Document(fromjson("{_id : {min : 2, max : 3}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[3], Document(fromjson("{_id : {min : 3, max : 4}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[4], Document(fromjson("{_id : {min : 4, max : 6}, count : 3}")));
}

TEST_F(BucketAutoTests, EvaluatesAccumulatorsInOutputField) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, output : {avg : {$avg : '$x'}}}}");
    auto results = getResults(
        bucketAutoSpec,
        {Document{{"x", 0}}, Document{{"x", 2}}, Document{{"x", 4}}, Document{{"x", 6}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 4}, avg : 1}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 4, max : 6}, avg : 5}")));
}

TEST_F(BucketAutoTests, EvaluatesNonFieldPathExpressionInGroupByField) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : {$add : ['$x', 1]}, buckets : 2}}");
    auto results = getResults(
        bucketAutoSpec,
        {Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 2}}, Document{{"x", 3}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 1, max : 3}, count : 2}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 3, max : 4}, count : 2}")));
}

TEST_F(BucketAutoTests, RespectsCanonicalTypeOrderingOfValues) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}");
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", "a"_sd}},
                               Document{{"x", 1}},
                               Document{{"x", "b"_sd}},
                               Document{{"x", 2}},
                               Document{{"x", 0.0}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0.0, max : 'a'}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 'a', max : 'b'}, count : 2}")));
}

TEST_F(BucketAutoTests, ShouldPropagatePauses) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}");
    auto mockStage =
        exec::agg::MockStage::createForTest({Document{{"x", 1}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"x", 2}},
                                             Document{{"x", 3}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"x", 4}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            getExpCtx());
    auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

    // The $bucketAuto stage needs to consume all inputs before returning any output, so we should
    // see all three pauses before any advances.
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());

    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       Document(fromjson("{_id : {min : 1, max : 3}, count : 2}")));

    next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       Document(fromjson("{_id : {min : 3, max : 4}, count : 2}")));

    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

TEST_F(BucketAutoTests, ShouldBeAbleToCorrectlySpillToDisk) {
    auto expCtx = getExpCtx();
    unittest::TempDir tempDir("DocumentSourceBucketAutoTest");
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);
    const size_t maxMemoryUsageBytes = 1000;
    RAIIServerParameterControllerForTest queryKnobController(
        "internalDocumentSourceBucketAutoMaxMemoryBytes",
        static_cast<long long>(maxMemoryUsageBytes));

    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mockStage =
        exec::agg::MockStage::createForTest({Document{{"a", 0}, {"largeStr", largeStr}},
                                             Document{{"a", 1}, {"largeStr", largeStr}},
                                             Document{{"a", 2}, {"largeStr", largeStr}},
                                             Document{{"a", 3}, {"largeStr", largeStr}}},
                                            expCtx);
    const int numBuckets = 2;
    auto bucketAutoStage = createBucketAutoStage(expCtx, groupByExpression, numBuckets, mockStage);

    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 0}, {"max", 2}}}, {"count", 2}}));

    next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 2}, {"max", 3}}}, {"count", 2}}));

    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
    ASSERT_TRUE(bucketAutoStage->usedDisk());

    ASSERT_EQ(bucketAutoStage->getMemoryTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(bucketAutoStage->getMemoryTracker_forTest()->peakTrackedMemoryBytes(), 0);

    auto stats =
        dynamic_cast<const DocumentSourceBucketAutoStats*>(bucketAutoStage->getSpecificStats());
    ASSERT_NE(stats, nullptr);
    ASSERT_EQ(stats->spillingStats.getSpills(), 7);
    ASSERT_EQ(stats->spillingStats.getSpilledRecords(), 13);
    ASSERT_EQ(stats->spillingStats.getSpilledBytes(), 13910);
    ASSERT_GT(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_LT(stats->spillingStats.getSpilledDataStorageSize(), 10000);
}

TEST_F(BucketAutoTests, ShouldBeAbleToPauseLoadingWhileSpilled) {
    auto expCtx = getExpCtx();

    unittest::TempDir tempDir("DocumentSourceBucketAutoTest");
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);
    const size_t maxMemoryUsageBytes = 1000;
    RAIIServerParameterControllerForTest queryKnobController(
        "internalDocumentSourceBucketAutoMaxMemoryBytes",
        static_cast<long long>(maxMemoryUsageBytes));

    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mockStage =
        exec::agg::MockStage::createForTest({Document{{"a", 0}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"a", 1}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"a", 2}, {"largeStr", largeStr}},
                                             Document{{"a", 3}, {"largeStr", largeStr}}},
                                            expCtx);

    const int numBuckets = 2;
    auto bucketAutoStage = createBucketAutoStage(expCtx, groupByExpression, numBuckets, mockStage);

    // There were 2 pauses, so we should expect 2 paused results before any results can be
    // returned.
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());

    // Now we expect to get the results back.
    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 0}, {"max", 2}}}, {"count", 2}}));

    next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 2}, {"max", 3}}}, {"count", 2}}));

    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
    ASSERT_TRUE(bucketAutoStage->usedDisk());
}

TEST_F(BucketAutoTests, ShouldBeAbleToForceSpillWhileLoadingDocuments) {
    auto expCtx = getExpCtx();

    unittest::TempDir tempDir("DocumentSourceBucketAutoTest");
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);

    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);

    string largeStr(1000, 'x');
    auto mockStage =
        exec::agg::MockStage::createForTest({Document{{"a", 0}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"a", 1}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"a", 2}, {"largeStr", largeStr}},
                                             Document{{"a", 3}, {"largeStr", largeStr}}},
                                            expCtx);
    const int numBuckets = 2;
    auto bucketAutoStage = createBucketAutoStage(expCtx, groupByExpression, numBuckets, mockStage);

    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    bucketAutoStage->forceSpill();
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());

    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 0}, {"max", 2}}}, {"count", 2}}));

    next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 2}, {"max", 3}}}, {"count", 2}}));

    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
    ASSERT_TRUE(bucketAutoStage->usedDisk());

    ASSERT_EQ(bucketAutoStage->getMemoryTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(bucketAutoStage->getMemoryTracker_forTest()->peakTrackedMemoryBytes(), 0);

    auto stats =
        dynamic_cast<const DocumentSourceBucketAutoStats*>(bucketAutoStage->getSpecificStats());
    ASSERT_NE(stats, nullptr);
    ASSERT_EQ(stats->spillingStats.getSpills(), 2);
    ASSERT_EQ(stats->spillingStats.getSpilledRecords(), 4);
    ASSERT_EQ(stats->spillingStats.getSpilledBytes(), 4280);
    ASSERT_GT(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_LT(stats->spillingStats.getSpilledDataStorageSize(),
              stats->spillingStats.getSpilledBytes());
}

TEST_F(BucketAutoTests, ShouldBeAbleToForceSpillWhileReturningDocuments) {
    auto expCtx = getExpCtx();

    unittest::TempDir tempDir("DocumentSourceBucketAutoTest");
    expCtx->setTempDir(tempDir.path());
    expCtx->setAllowDiskUse(true);

    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);

    string largeStr(1000, 'x');
    auto mockStage =
        exec::agg::MockStage::createForTest({Document{{"a", 0}, {"largeStr", largeStr}},
                                             Document{{"a", 1}, {"largeStr", largeStr}},
                                             Document{{"a", 2}, {"largeStr", largeStr}},
                                             Document{{"a", 3}, {"largeStr", largeStr}}},
                                            expCtx);

    const int numBuckets = 2;
    auto bucketAutoStage = createBucketAutoStage(expCtx, groupByExpression, numBuckets, mockStage);

    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 0}, {"max", 2}}}, {"count", 2}}));

    bucketAutoStage->forceSpill();

    next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 2}, {"max", 3}}}, {"count", 2}}));

    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
    ASSERT_TRUE(bucketAutoStage->usedDisk());

    ASSERT_EQ(bucketAutoStage->getMemoryTracker_forTest()->inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(bucketAutoStage->getMemoryTracker_forTest()->peakTrackedMemoryBytes(), 0);

    auto stats =
        dynamic_cast<const DocumentSourceBucketAutoStats*>(bucketAutoStage->getSpecificStats());
    ASSERT_NE(stats, nullptr);
    ASSERT_EQ(stats->spillingStats.getSpills(), 1);
    ASSERT_EQ(stats->spillingStats.getSpilledRecords(), 1);
    ASSERT_EQ(stats->spillingStats.getSpilledBytes(), 1070);
    ASSERT_GT(stats->spillingStats.getSpilledDataStorageSize(), 0);
    ASSERT_LT(stats->spillingStats.getSpilledDataStorageSize(),
              stats->spillingStats.getSpilledBytes());
}

TEST_F(BucketAutoTests, SourceNameIsBucketAuto) {
    auto bucketAuto = createBucketAuto(fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}"));
    ASSERT_EQUALS(string(bucketAuto->getSourceName()), "$bucketAuto");
}

TEST_F(BucketAutoTests, ShouldAddDependenciesOfGroupByFieldAndComputedFields) {
    auto bucketAuto =
        createBucketAuto(fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, output: {field1 : "
                                  "{$sum : '$a'}, field2 : {$avg : '$b'}}}}"));

    DepsTracker dependencies;
    ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_ALL, bucketAuto->getDependencies(&dependencies));
    ASSERT_EQUALS(3U, dependencies.fields.size());

    // Dependency from 'groupBy'
    ASSERT_EQUALS(1U, dependencies.fields.count("x"));

    // Dependencies from 'output'
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));

    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(BucketAutoTests, ShouldNeedTextScoreInDependenciesFromGroupByField) {
    auto bucketAuto =
        createBucketAuto(fromjson("{$bucketAuto : {groupBy : {$meta: 'textScore'}, buckets : 2}}"));

    DepsTracker dependencies(DepsTracker::kOnlyTextScore);
    ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_ALL, bucketAuto->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());

    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(BucketAutoTests, ShouldNeedTextScoreInDependenciesFromOutputField) {
    auto bucketAuto =
        createBucketAuto(fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, output: {avg : "
                                  "{$avg : {$meta : 'textScore'}}}}}"));

    DepsTracker dependencies(DepsTracker::kOnlyTextScore);
    ASSERT_EQUALS(DepsTracker::State::EXHAUSTIVE_ALL, bucketAuto->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.size());

    // Dependency from 'groupBy'
    ASSERT_EQUALS(1U, dependencies.fields.count("x"));

    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(BucketAutoTests, SerializesDefaultAccumulatorIfOutputFieldIsNotSpecified) {
    BSONObj spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}");
    BSONObj expected =
        fromjson("{groupBy : '$x', buckets : 2, output : {count : {$sum : {$const : 1}}}}");

    testSerialize(spec, expected);
}

TEST_F(BucketAutoTests, SerializesOutputFieldIfSpecified) {
    BSONObj spec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, output : {field : {$avg : '$x'}}}}");
    BSONObj expected = fromjson("{groupBy : '$x', buckets : 2, output : {field : {$avg : '$x'}}}");

    testSerialize(spec, expected);
}

TEST_F(BucketAutoTests, SerializesGranularityFieldIfSpecified) {
    BSONObj spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");
    BSONObj expected = fromjson(
        "{groupBy : '$x', buckets : 2, granularity : 'R5', output : {count : {$sum : {$const : "
        "1}}}}");

    testSerialize(spec, expected);
}

TEST_F(BucketAutoTests, ShouldBeAbleToReParseSerializedStage) {
    auto bucketAuto =
        createBucketAuto(fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity: 'R5', "
                                  "output : {field : {$avg : '$x'}}}}"));
    vector<Value> serialization;
    bucketAuto->serializeToArray(serialization);
    ASSERT_EQUALS(serialization.size(), 1UL);
    ASSERT_EQUALS(serialization[0].getType(), BSONType::object);

    ASSERT_EQUALS(serialization[0].getDocument().computeSize(), 1ULL);
    ASSERT_EQUALS(serialization[0].getDocument()["$bucketAuto"].getType(), BSONType::object);

    auto serializedBson = serialization[0].getDocument().toBson();
    auto roundTripped = createBucketAuto(serializedBson);

    vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQUALS(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(BucketAutoTests, FailsWithInvalidNumberOfBuckets) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 'test'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40241);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2147483648}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40242);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1.5}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40242);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 0}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40243);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : -1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40243);

    // Use the create() helper.
    const int numBuckets = 0;
    ASSERT_THROWS_CODE(
        DocumentSourceBucketAuto::create(
            getExpCtx(), ExpressionConstant::create(getExpCtxRaw(), Value(0)), numBuckets),
        AssertionException,
        40243);
}

TEST_F(BucketAutoTests, FailsWithNonOrInvalidExpressionGroupBy) {
    auto spec = fromjson("{$bucketAuto : {groupBy : 'test', buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : {test : 'test'}, buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : '', buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : {}}, buckets : 1}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : '$'}, buckets : 1}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : []}, buckets : 1}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : null}, buckets : 1}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40239);
}

TEST_F(BucketAutoTests, FailsWithNonObjectArgument) {
    auto spec = fromjson("{$bucketAuto : 'test'}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40240);

    spec = fromjson("{$bucketAuto : [1, 2, 3]}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40240);
}

TEST_F(BucketAutoTests, FailsWithNonObjectOutput) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : 'test'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40244);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : [1, 2, 3]}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40244);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40244);
}

TEST_F(BucketAutoTests, FailsWhenGroupByMissing) {
    auto spec = fromjson("{$bucketAuto : {buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40246);
}

TEST_F(BucketAutoTests, FailsWhenBucketsMissing) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40246);
}

TEST_F(BucketAutoTests, FailsWithUnknownField) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, field : 'test'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40245);
}

TEST_F(BucketAutoTests, FailsWithInvalidExpressionToAccumulator) {
    auto spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {avg : {$avg : ['$x', 1]}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40237);

    spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {test : {$avg : '$x', $sum : "
        "'$x'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40238);
}

TEST_F(BucketAutoTests, FailsWithNonAccumulatorObjectOutputField) {
    auto spec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : {field : 'test'}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40234);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : {field : 1}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40234);

    spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {test : {field : 'test'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40234);
}

TEST_F(BucketAutoTests, FailsWithInvalidOutputFieldName) {
    auto spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {'field.test' : {$avg : '$x'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40235);

    spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {'$field' : {$avg : '$x'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), AssertionException, 40236);
}

void assertCannotSpillToDisk(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const size_t maxMemoryUsageBytes = 1000;
    RAIIServerParameterControllerForTest queryKnobController(
        "internalDocumentSourceBucketAutoMaxMemoryBytes",
        static_cast<long long>(maxMemoryUsageBytes));

    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mockStage = exec::agg::MockStage::createForTest(
        {Document{{"a", 0}, {"largeStr", largeStr}}, Document{{"a", 1}, {"largeStr", largeStr}}},
        expCtx);

    const int numBuckets = 2;
    auto bucketAutoDS = DocumentSourceBucketAuto::create(expCtx, groupByExpression, numBuckets);
    auto bucketAutoStage = exec::agg::buildStageAndStitch(bucketAutoDS, mockStage);

    ASSERT_THROWS_CODE(bucketAutoStage->getNext(),
                       AssertionException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(BucketAutoTests, ShouldFailIfBufferingTooManyDocuments) {
    auto expCtx = getExpCtx();

    expCtx->setAllowDiskUse(false);
    expCtx->setInRouter(false);
    assertCannotSpillToDisk(expCtx);

    expCtx->setAllowDiskUse(true);
    expCtx->setInRouter(true);
    assertCannotSpillToDisk(expCtx);

    expCtx->setAllowDiskUse(false);
    expCtx->setInRouter(true);
    assertCannotSpillToDisk(expCtx);
}

TEST_F(BucketAutoTests, ShouldCorrectlyTrackMemoryUsageBetweenPauses) {
    auto expCtx = getExpCtx();
    expCtx->setAllowDiskUse(false);
    const size_t maxMemoryUsageBytes = 2000;
    RAIIServerParameterControllerForTest queryKnobController(
        "internalDocumentSourceBucketAutoMaxMemoryBytes",
        static_cast<long long>(maxMemoryUsageBytes));

    VariablesParseState vps = expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(expCtx.get(), "$a", vps);

    string largeStr(maxMemoryUsageBytes / 5, 'x');
    auto mockStage =
        exec::agg::MockStage::createForTest({Document{{"a", 0}, {"largeStr", largeStr}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"a", 1}, {"largeStr", largeStr}},
                                             Document{{"a", 2}, {"largeStr", largeStr}}},
                                            expCtx);
    const int numBuckets = 2;
    auto bucketAutoStage = createBucketAutoStage(expCtx, groupByExpression, numBuckets, mockStage);

    // The first getNext() should pause.
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());

    // The next should realize it's used too much memory.
    ASSERT_THROWS_CODE(bucketAutoStage->getNext(),
                       AssertionException,
                       ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed);
}

TEST_F(BucketAutoTests, ShouldRoundUpMaximumBoundariesWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    // Values are 0, 15, 24, 30, 50
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 24}},
                               Document{{"x", 15}},
                               Document{{"x", 30}},
                               Document{{"x", 50}},
                               Document{{"x", 0}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 25}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 25, max : 63}, count : 2}")));
}

TEST_F(BucketAutoTests, ShouldRoundDownFirstMinimumBoundaryWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    // Values are 1, 15, 24, 30, 50
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 24}},
                               Document{{"x", 15}},
                               Document{{"x", 30}},
                               Document{{"x", 50}},
                               Document{{"x", 1}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0.63, max : 25}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 25, max : 63}, count : 2}")));
}

TEST_F(BucketAutoTests, PopulatesLastBucketWithRemainingDocumentsWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 5, granularity : 'R5'}}");
    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 24}},
                               Document{{"x", 15}},
                               Document{{"x", 30}},
                               Document{{"x", 9}},
                               Document{{"x", 3}},
                               Document{{"x", 7}},
                               Document{{"x", 101}}});
    ASSERT_EQUALS(results.size(), 5UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min: 2.5, max: 4.0}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 4.0, max : 10}, count : 2}")));
    ASSERT_DOCUMENT_EQ(results[2], Document(fromjson("{_id : {min : 10, max : 16}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[3], Document(fromjson("{_id : {min : 16, max : 25}, count : 1}")));
    ASSERT_DOCUMENT_EQ(results[4], Document(fromjson("{_id : {min : 25, max : 160}, count : 2}")));
}

TEST_F(BucketAutoTests, ShouldAbsorbAllValuesSmallerThanAdjustedBoundaryWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 0}},
                               Document{{"x", 5}},
                               Document{{"x", 10}},
                               Document{{"x", 15}},
                               Document{{"x", 30}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 16}, count : 4}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 16, max : 40}, count : 1}")));
}

TEST_F(BucketAutoTests, ShouldBeAbleToAbsorbAllValuesIntoOneBucketWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 0}},
                               Document{{"x", 5}},
                               Document{{"x", 10}},
                               Document{{"x", 14}},
                               Document{{"x", 15}}});

    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 16}, count : 5}")));
}

TEST_F(BucketAutoTests, ShouldNotRoundZeroInFirstBucketWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(
        bucketAutoSpec,
        {Document{{"x", 0}}, Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 1}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 0.63}, count : 2}")));
    ASSERT_DOCUMENT_EQ(results[1],
                       Document(fromjson("{_id : {min : 0.63, max : 1.6}, count : 2}")));
}

TEST_F(BucketAutoTests, AllValuesZeroWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(
        bucketAutoSpec,
        {Document{{"x", 0}}, Document{{"x", 0}}, Document{{"x", 0}}, Document{{"x", 0}}});

    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 0}, count : 4}")));
}

TEST_F(BucketAutoTests, MostValuesZeroWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(
        bucketAutoSpec,
        {Document{{"x", 0}}, Document{{"x", 0}}, Document{{"x", 0}}, Document{{"x", 1}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0, max : 0.63}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1],
                       Document(fromjson("{_id : {min : 0.63, max : 1.6}, count : 1}")));
}

TEST_F(BucketAutoTests, AllValuesInfinityWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", std::numeric_limits<double>::infinity()}},
                               Document{{"x", std::numeric_limits<double>::infinity()}},
                               Document{{"x", std::numeric_limits<double>::infinity()}},
                               Document{{"x", std::numeric_limits<double>::infinity()}}});

    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0],
                       Document(fromjson("{_id : {min : Infinity, max : Infinity}, count : 4}")));
}

TEST_F(BucketAutoTests, MostValuesInfinityWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 1}},
                               Document{{"x", std::numeric_limits<double>::infinity()}},
                               Document{{"x", std::numeric_limits<double>::infinity()}},
                               Document{{"x", std::numeric_limits<double>::infinity()}}});

    ASSERT_EQUALS(results.size(), 1UL);
    ASSERT_DOCUMENT_EQ(results[0],
                       Document(fromjson("{_id : {min : 0.63, max : Infinity}, count : 4}")));
}

TEST_F(BucketAutoTests, OneValueInfinityWithGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    auto results = getResults(bucketAutoSpec,
                              {Document{{"x", 1}},
                               Document{{"x", 1}},
                               Document{{"x", 1}},
                               Document{{"x", std::numeric_limits<double>::infinity()}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0],
                       Document(fromjson("{_id : {min : 0.63, max : 1.6}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1],
                       Document(fromjson("{_id : {min : 1.6, max : Infinity}, count : 1}")));
}


TEST_F(BucketAutoTests, ShouldFailOnNaNWhenGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    ASSERT_THROWS_CODE(getResults(bucketAutoSpec,
                                  {Document{{"x", 0}},
                                   Document{{"x", std::nan("NaN")}},
                                   Document{{"x", 1}},
                                   Document{{"x", 1}}}),
                       AssertionException,
                       40259);
}

TEST_F(BucketAutoTests, ShouldFailOnNonNumericValuesWhenGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    ASSERT_THROWS_CODE(getResults(bucketAutoSpec,
                                  {Document{{"x", 0}},
                                   Document{{"x", "test"_sd}},
                                   Document{{"x", 1}},
                                   Document{{"x", 1}}}),
                       AssertionException,
                       40258);
}

TEST_F(BucketAutoTests, ShouldFailOnNegativeNumbersWhenGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    ASSERT_THROWS_CODE(
        getResults(
            bucketAutoSpec,
            {Document{{"x", 0}}, Document{{"x", -1}}, Document{{"x", 1}}, Document{{"x", 2}}}),
        AssertionException,
        40260);
}

TEST_F(BucketAutoTests, RedactionWithoutOutputField) {
    auto spec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 5,
                granularity: "R5"
            }
        })");
    auto docSource = DocumentSourceBucketAuto::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$bucketAuto": {
                "groupBy": "$HASH<_id>",
                "buckets": "?number",
                "granularity": "?string",
                "output": {
                    "HASH<count>": {
                        "$sum": "?number"
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(BucketAutoTests, RedactionWithOutputField) {
    auto spec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$year',
                buckets: 3,
                output: {
                    count: { $sum: 1 },
                    years: { $push: '$year' }
                }
            }})");
    auto docSource = DocumentSourceBucketAuto::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$bucketAuto": {
                "groupBy": "$HASH<year>",
                "buckets": "?number",
                "output": {
                    "HASH<count>": {
                        "$sum": "?number"
                    },
                    "HASH<years>": {
                        "$push": "$HASH<year>"
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(BucketAutoTests, QueryShapeReParseSerializedStage) {
    auto expCtx = getExpCtx();
    auto spec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$year',
                buckets: 3,
                granularity: "E192",
                output: {
                    count: { $sum: 1 },
                    years: { $push: '$year' }
                }
            }})");

    auto docSource = DocumentSourceBucketAuto::createFromBson(spec.firstElement(), expCtx);
    auto opts = SerializationOptions{LiteralSerializationPolicy::kToRepresentativeParseableValue};
    std::vector<Value> serialized;
    docSource->serializeToArray(serialized, opts);
    auto serializedDocSource = serialized[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$bucketAuto": {
                "groupBy": "$year",
                "buckets": 1,
                "granularity": "R5",
                "output": {
                    "count": {
                        "$sum": {
                            "$const":1
                        }
                    },
                    "years": {
                        "$push": "$year"
                    }
                }
            }
        })",
        serializedDocSource);
    auto docSourceFromQueryShape =
        DocumentSourceBucketAuto::createFromBson(serializedDocSource.firstElement(), expCtx);

    vector<Value> newSerialization;
    docSourceFromQueryShape->serializeToArray(newSerialization, opts);
    auto newSerializedDocSource = newSerialization[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedDocSource, newSerializedDocSource);
}

TEST_F(BucketAutoTests, BucketAutoWithPushRespectsMemoryLimit) {
    auto spec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    array: { $push: '$arr' }
                }
            }})");

    // In non-debug modes, each array in the 100 documents below takes up roughly 110 bytes. The
    // infrastructure that processes each new element in the accumulator takes up roughly another
    // 100 bytes. So we should require roughly 100 * 110 + 100 bytes for this operation to succeed,
    // but lets round up to the nearest 500. In debug modes, each array can take up to ~136 bytes as
    // an upper bound, while the infrastructure that processes each element in the accumulator takes
    // up ~120 bytes. We should require 136 * 100 + 120 bytes for this operation to succeed, but
    // we'll round up to the nearest 500 for buffer.
    RAIIServerParameterControllerForTest queryKnobController("internalQueryMaxPushBytes",
                                                             kDebugBuild ? 14000 : 11500);
    std::deque<Document> docs;
    for (size_t i = 0; i < 100; i++) {
        docs.push_back(
            Document(fromjson(str::stream() << "{_id : " << i << ", arr : ['string 0']}")));
    }
    auto results = getResults(spec, docs);
    // Assert on the result size (1 bucket expected) to confirm we didn't run out of memory.
    ASSERT_EQUALS(results.size(), 1UL);

    // Decrease the memory limit and run again, asserting that we hit an error.
    RAIIServerParameterControllerForTest queryKnobController2("internalQueryMaxPushBytes", 9600);
    ASSERT_THROWS_CODE(getResults(spec, docs), AssertionException, ErrorCodes::ExceededMemoryLimit);
}

TEST_F(BucketAutoTests, BucketAutoWithConcatArraysRespectsMemoryLimit) {
    auto spec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    array: { $concatArrays: '$arr' }
                }
            }})");

    // In non-debug modes, each array in the 100 documents below takes up roughly 110 bytes. The
    // infrastructure that processes each new element in the accumulator takes up roughly another
    // 100 bytes. So we should require roughly 100 * 110 + 100 bytes for this operation to succeed,
    // but lets round up to the nearest 500. In debug modes, each array can take up to ~136 bytes as
    // an upper bound, while the infrastructure that processes each element in the accumulator takes
    // up ~120 bytes. We should require 136 * 100 + 120 bytes for this operation to succeed, but
    // we'll round up to the nearest 500 for buffer.
    RAIIServerParameterControllerForTest queryKnobController("internalQueryMaxConcatArraysBytes",
                                                             kDebugBuild ? 14000 : 11500);
    std::deque<Document> docs;
    for (size_t i = 0; i < 100; i++) {
        docs.push_back(
            Document(fromjson(str::stream() << "{_id : " << i << ", arr : ['string 0']}")));
    }
    auto results = getResults(spec, docs);
    // Assert on the result size (1 bucket expected) to confirm we didn't run out of memory.
    ASSERT_EQUALS(results.size(), 1UL);

    // Decrease the memory limit and run again, asserting that we hit an error.
    RAIIServerParameterControllerForTest queryKnobController2("internalQueryMaxConcatArraysBytes",
                                                              9600);
    ASSERT_THROWS_CODE(getResults(spec, docs), AssertionException, ErrorCodes::ExceededMemoryLimit);
}

TEST_F(BucketAutoTests, PauseBucketAutoWithConcatArrays) {
    auto bucketAutoSpec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    array: { $concatArrays: '$arr' }
                }
            }})");
    deque<DocumentSource::GetNextResult> mockInputs{
        Document(fromjson("{_id: 0, arr: ['string 0']}")),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document(fromjson("{_id: 1, arr: ['string 1']}"))};
    auto mockStage = exec::agg::MockStage::createForTest(std::move(mockInputs), getExpCtx());
    auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(
        doc, Document(fromjson("{_id: {min: 0, max: 1}, array: ['string 0', 'string 1']}")));
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

TEST_F(BucketAutoTests, PauseBucketAutoWithPush) {
    auto bucketAutoSpec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    array: { $push: '$arr' }
                }
            }})");
    deque<DocumentSource::GetNextResult> mockInputs{
        Document(fromjson("{_id: 0, arr: 'string 0'}")),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document(fromjson("{_id: 1, arr: 'string 1'}"))};
    auto mockStage = exec::agg::MockStage::createForTest(std::move(mockInputs), getExpCtx());
    auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(
        doc, Document(fromjson("{_id: {min: 0, max: 1}, array: ['string 0', 'string 1']}")));
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

TEST_F(BucketAutoTests, PauseBucketAutoWithMergeObjects) {
    auto bucketAutoSpec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    obj: { $mergeObjects: '$o' }
                }
            }})");
    deque<DocumentSource::GetNextResult> mockInputs{
        Document(fromjson("{_id: 0, o: {a: 1}}")),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document(fromjson("{_id: 1, o: {a: 2}}"))};
    auto mockStage = exec::agg::MockStage::createForTest(std::move(mockInputs), getExpCtx());
    auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(doc, Document(fromjson("{_id: {min: 0, max: 1}, obj: {a: 2}}")));
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

TEST_F(BucketAutoTests, PauseBucketAutoWithFirstN) {
    auto bucketAutoSpec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    foo: { $firstN: {input: '$a', n: 2 }}
                }
            }})");
    deque<DocumentSource::GetNextResult> mockInputs{
        Document(fromjson("{_id: 0, a: 1}")),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document(fromjson("{_id: 1, a: 2}")),
        Document(fromjson("{_id: 1, a: 3}"))};
    auto mockStage = exec::agg::MockStage::createForTest(std::move(mockInputs), getExpCtx());
    auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(doc, Document(fromjson("{_id: {min: 0, max: 1}, foo: [1, 2]}")));
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

TEST_F(BucketAutoTests, PauseBucketAutoWithLastN) {
    auto bucketAutoSpec = fromjson(R"({
            $bucketAuto: {
                groupBy: '$_id',
                buckets: 1,
                output: {
                    foo: { $lastN: {input: '$a', n: 2 }}
                }
            }})");
    deque<DocumentSource::GetNextResult> mockInputs{
        Document(fromjson("{_id: 0, a: 1}")),
        Document(fromjson("{_id: 1, a: 2}")),
        DocumentSource::GetNextResult::makePauseExecution(),
        Document(fromjson("{_id: 1, a: 3}"))};
    auto mockStage = exec::agg::MockStage::createForTest(std::move(mockInputs), getExpCtx());
    auto bucketAutoStage = createBucketAutoStage(bucketAutoSpec, mockStage);

    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());
    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    auto doc = next.releaseDocument();
    ASSERT_DOCUMENT_EQ(doc, Document(fromjson("{_id: {min: 0, max: 1}, foo: [2, 3]}")));
    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo

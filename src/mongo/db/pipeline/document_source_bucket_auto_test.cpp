/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_bucket_auto.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using std::deque;
using std::vector;
using std::string;
using boost::intrusive_ptr;

class BucketAutoTests : public AggregationContextFixture {
public:
    intrusive_ptr<DocumentSource> createBucketAuto(BSONObj bucketAutoSpec) {
        return DocumentSourceBucketAuto::createFromBson(bucketAutoSpec.firstElement(), getExpCtx());
    }

    vector<Document> getResults(BSONObj bucketAutoSpec, deque<Document> inputs) {
        auto bucketAutoStage = createBucketAuto(bucketAutoSpec);
        assertBucketAutoType(bucketAutoStage);

        // Convert Documents to GetNextResults.
        deque<DocumentSource::GetNextResult> mockInputs;
        for (auto&& input : inputs) {
            mockInputs.emplace_back(std::move(input));
        }

        auto source = DocumentSourceMock::create(std::move(mockInputs));
        bucketAutoStage->setSource(source.get());

        vector<Document> results;
        for (auto next = bucketAutoStage->getNext(); next.isAdvanced();
             next = bucketAutoStage->getNext()) {
            results.push_back(next.releaseDocument());
        }

        return results;
    }

    void testSerialize(BSONObj bucketAutoSpec, BSONObj expectedObj) {
        auto bucketAutoStage = createBucketAuto(bucketAutoSpec);
        assertBucketAutoType(bucketAutoStage);

        const bool explain = true;
        vector<Value> explainedStages;
        bucketAutoStage->serializeToArray(explainedStages, explain);
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
};

TEST_F(BucketAutoTests, ReturnsNoBucketsWhenSourceIsEmpty) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets: 1}}");
    auto results = getResults(bucketAutoSpec, {});
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
    results = getResults(
        bucketAutoSpec,
        {Document{{"x", "d"}}, Document{{"x", "b"}}, Document{{"x", "a"}}, Document{{"x", "c"}}});
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

    results = getResults(bucketAutoSpec, {Document{{"x", "a"}}});
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
                              {Document{{"x", "a"}},
                               Document{{"x", 1}},
                               Document{{"x", "b"}},
                               Document{{"x", 2}},
                               Document{{"x", 0.0}}});

    ASSERT_EQUALS(results.size(), 2UL);
    ASSERT_DOCUMENT_EQ(results[0], Document(fromjson("{_id : {min : 0.0, max : 'a'}, count : 3}")));
    ASSERT_DOCUMENT_EQ(results[1], Document(fromjson("{_id : {min : 'a', max : 'b'}, count : 2}")));
}

TEST_F(BucketAutoTests, ShouldPropagatePauses) {
    auto bucketAutoSpec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2}}");
    auto bucketAutoStage = createBucketAuto(bucketAutoSpec);
    auto source = DocumentSourceMock::create({Document{{"x", 1}},
                                              DocumentSource::GetNextResult::makePauseExecution(),
                                              Document{{"x", 2}},
                                              Document{{"x", 3}},
                                              DocumentSource::GetNextResult::makePauseExecution(),
                                              Document{{"x", 4}},
                                              DocumentSource::GetNextResult::makePauseExecution()});
    bucketAutoStage->setSource(source.get());

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
    expCtx->tempDir = tempDir.path();
    expCtx->extSortAllowed = true;
    const size_t maxMemoryUsageBytes = 1000;

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    auto groupByExpression = ExpressionFieldPath::parse("$a", vps);

    const int numBuckets = 2;
    auto bucketAutoStage = DocumentSourceBucketAuto::create(expCtx,
                                                            groupByExpression,
                                                            idGen.getIdCount(),
                                                            numBuckets,
                                                            {},
                                                            nullptr,
                                                            maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mock = DocumentSourceMock::create({Document{{"a", 0}, {"largeStr", largeStr}},
                                            Document{{"a", 1}, {"largeStr", largeStr}},
                                            Document{{"a", 2}, {"largeStr", largeStr}},
                                            Document{{"a", 3}, {"largeStr", largeStr}}});
    bucketAutoStage->setSource(mock.get());

    auto next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 0}, {"max", 2}}}, {"count", 2}}));

    next = bucketAutoStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"_id", Document{{"min", 2}, {"max", 3}}}, {"count", 2}}));

    ASSERT_TRUE(bucketAutoStage->getNext().isEOF());
}

TEST_F(BucketAutoTests, ShouldBeAbleToPauseLoadingWhileSpilled) {
    auto expCtx = getExpCtx();

    // Allow the $sort stage to spill to disk.
    unittest::TempDir tempDir("DocumentSourceBucketAutoTest");
    expCtx->tempDir = tempDir.path();
    expCtx->extSortAllowed = true;
    const size_t maxMemoryUsageBytes = 1000;

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    auto groupByExpression = ExpressionFieldPath::parse("$a", vps);

    const int numBuckets = 2;
    auto bucketAutoStage = DocumentSourceBucketAuto::create(expCtx,
                                                            groupByExpression,
                                                            idGen.getIdCount(),
                                                            numBuckets,
                                                            {},
                                                            nullptr,
                                                            maxMemoryUsageBytes);
    auto sort = DocumentSourceSort::create(expCtx, BSON("_id" << -1), -1, maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mock = DocumentSourceMock::create({Document{{"a", 0}, {"largeStr", largeStr}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"a", 1}, {"largeStr", largeStr}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"a", 2}, {"largeStr", largeStr}},
                                            Document{{"a", 3}, {"largeStr", largeStr}}});
    bucketAutoStage->setSource(mock.get());

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
    ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_ALL, bucketAuto->getDependencies(&dependencies));
    ASSERT_EQUALS(3U, dependencies.fields.size());

    // Dependency from 'groupBy'
    ASSERT_EQUALS(1U, dependencies.fields.count("x"));

    // Dependencies from 'output'
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));

    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(BucketAutoTests, ShouldNeedTextScoreInDependenciesFromGroupByField) {
    auto bucketAuto =
        createBucketAuto(fromjson("{$bucketAuto : {groupBy : {$meta: 'textScore'}, buckets : 2}}"));

    DepsTracker dependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_ALL, bucketAuto->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());

    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedTextScore());
}

TEST_F(BucketAutoTests, ShouldNeedTextScoreInDependenciesFromOutputField) {
    auto bucketAuto =
        createBucketAuto(fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, output: {avg : "
                                  "{$avg : {$meta : 'textScore'}}}}}"));

    DepsTracker dependencies(DepsTracker::MetadataAvailable::kTextScore);
    ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_ALL, bucketAuto->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.size());

    // Dependency from 'groupBy'
    ASSERT_EQUALS(1U, dependencies.fields.count("x"));

    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedTextScore());
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
    ASSERT_EQUALS(serialization[0].getType(), BSONType::Object);

    ASSERT_EQUALS(serialization[0].getDocument().size(), 1UL);
    ASSERT_EQUALS(serialization[0].getDocument()["$bucketAuto"].getType(), BSONType::Object);

    auto serializedBson = serialization[0].getDocument().toBson();
    auto roundTripped = createBucketAuto(serializedBson);

    vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQUALS(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(BucketAutoTests, FailsWithInvalidNumberOfBuckets) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 'test'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40241);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2147483648}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40242);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1.5}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40242);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 0}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40243);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : -1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40243);

    // Use the create() helper.
    const int numBuckets = 0;
    ASSERT_THROWS_CODE(
        DocumentSourceBucketAuto::create(
            getExpCtx(), ExpressionConstant::create(getExpCtx(), Value(0)), 0, numBuckets),
        UserException,
        40243);
}

TEST_F(BucketAutoTests, FailsWithNonExpressionGroupBy) {
    auto spec = fromjson("{$bucketAuto : {groupBy : 'test', buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40239);

    spec = fromjson("{$bucketAuto : {groupBy : {test : 'test'}, buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40239);
}

TEST_F(BucketAutoTests, FailsWithNonObjectArgument) {
    auto spec = fromjson("{$bucketAuto : 'test'}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40240);

    spec = fromjson("{$bucketAuto : [1, 2, 3]}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40240);
}

TEST_F(BucketAutoTests, FailsWithNonObjectOutput) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : 'test'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40244);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : [1, 2, 3]}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40244);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40244);
}

TEST_F(BucketAutoTests, FailsWhenGroupByMissing) {
    auto spec = fromjson("{$bucketAuto : {buckets : 1}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40246);
}

TEST_F(BucketAutoTests, FailsWhenBucketsMissing) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40246);
}

TEST_F(BucketAutoTests, FailsWithUnknownField) {
    auto spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, field : 'test'}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40245);
}

TEST_F(BucketAutoTests, FailsWithInvalidExpressionToAccumulator) {
    auto spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {avg : {$avg : ['$x', 1]}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40237);

    spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {test : {$avg : '$x', $sum : "
        "'$x'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40238);
}

TEST_F(BucketAutoTests, FailsWithNonAccumulatorObjectOutputField) {
    auto spec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : {field : 'test'}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40234);

    spec = fromjson("{$bucketAuto : {groupBy : '$x', buckets : 1, output : {field : 1}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40234);

    spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {test : {field : 'test'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40234);
}

TEST_F(BucketAutoTests, FailsWithInvalidOutputFieldName) {
    auto spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {'field.test' : {$avg : '$x'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40235);

    spec = fromjson(
        "{$bucketAuto : {groupBy : '$x', buckets : 1, output : {'$field' : {$avg : '$x'}}}}");
    ASSERT_THROWS_CODE(createBucketAuto(spec), UserException, 40236);
}

void assertCannotSpillToDisk(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const size_t maxMemoryUsageBytes = 1000;

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    auto groupByExpression = ExpressionFieldPath::parse("$a", vps);

    const int numBuckets = 2;
    auto bucketAutoStage = DocumentSourceBucketAuto::create(expCtx,
                                                            groupByExpression,
                                                            idGen.getIdCount(),
                                                            numBuckets,
                                                            {},
                                                            nullptr,
                                                            maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes, 'x');
    auto mock = DocumentSourceMock::create(
        {Document{{"a", 0}, {"largeStr", largeStr}}, Document{{"a", 1}, {"largeStr", largeStr}}});
    bucketAutoStage->setSource(mock.get());

    ASSERT_THROWS_CODE(bucketAutoStage->getNext(), UserException, 16819);
}

TEST_F(BucketAutoTests, ShouldFailIfBufferingTooManyDocuments) {
    auto expCtx = getExpCtx();

    expCtx->extSortAllowed = false;
    expCtx->inRouter = false;
    assertCannotSpillToDisk(expCtx);

    expCtx->extSortAllowed = true;
    expCtx->inRouter = true;
    assertCannotSpillToDisk(expCtx);

    expCtx->extSortAllowed = false;
    expCtx->inRouter = true;
    assertCannotSpillToDisk(expCtx);
}

TEST_F(BucketAutoTests, ShouldCorrectlyTrackMemoryUsageBetweenPauses) {
    auto expCtx = getExpCtx();
    expCtx->extSortAllowed = false;
    const size_t maxMemoryUsageBytes = 1000;

    VariablesIdGenerator idGen;
    VariablesParseState vps(&idGen);
    auto groupByExpression = ExpressionFieldPath::parse("$a", vps);

    const int numBuckets = 2;
    auto bucketAutoStage = DocumentSourceBucketAuto::create(expCtx,
                                                            groupByExpression,
                                                            idGen.getIdCount(),
                                                            numBuckets,
                                                            {},
                                                            nullptr,
                                                            maxMemoryUsageBytes);

    string largeStr(maxMemoryUsageBytes / 2, 'x');
    auto mock = DocumentSourceMock::create({Document{{"a", 0}, {"largeStr", largeStr}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"a", 1}, {"largeStr", largeStr}},
                                            Document{{"a", 2}, {"largeStr", largeStr}}});
    bucketAutoStage->setSource(mock.get());

    // The first getNext() should pause.
    ASSERT_TRUE(bucketAutoStage->getNext().isPaused());

    // The next should realize it's used too much memory.
    ASSERT_THROWS_CODE(bucketAutoStage->getNext(), UserException, 16819);
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

TEST_F(BucketAutoTests, ShouldFailOnNaNWhenGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    ASSERT_THROWS_CODE(getResults(bucketAutoSpec,
                                  {Document{{"x", 0}},
                                   Document{{"x", std::nan("NaN")}},
                                   Document{{"x", 1}},
                                   Document{{"x", 1}}}),
                       UserException,
                       40259);
}

TEST_F(BucketAutoTests, ShouldFailOnNonNumericValuesWhenGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    ASSERT_THROWS_CODE(
        getResults(
            bucketAutoSpec,
            {Document{{"x", 0}}, Document{{"x", "test"}}, Document{{"x", 1}}, Document{{"x", 1}}}),
        UserException,
        40258);
}

TEST_F(BucketAutoTests, ShouldFailOnNegativeNumbersWhenGranularitySpecified) {
    auto bucketAutoSpec =
        fromjson("{$bucketAuto : {groupBy : '$x', buckets : 2, granularity : 'R5'}}");

    ASSERT_THROWS_CODE(
        getResults(
            bucketAutoSpec,
            {Document{{"x", 0}}, Document{{"x", -1}}, Document{{"x", 1}}, Document{{"x", 2}}}),
        UserException,
        40260);
}
}  // namespace
}  // namespace mongo

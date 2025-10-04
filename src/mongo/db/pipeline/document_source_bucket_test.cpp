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

#include "mongo/db/pipeline/document_source_bucket.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "src/mongo/db/exec/agg/document_source_to_stage_registry.h"
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using boost::intrusive_ptr;
using std::list;
using std::vector;

class BucketReturnsGroupAndSort : public AggregationContextFixture {
public:
    void testCreateFromBsonResult(BSONObj bucketSpec,
                                  Value expectedGroupExplain,
                                  bool toOptimize = false) {
        list<intrusive_ptr<DocumentSource>> result =
            DocumentSourceBucket::createFromBson(bucketSpec.firstElement(), getExpCtx());

        ASSERT_EQUALS(result.size(), 2UL);

        if (toOptimize) {
            std::transform(result.begin(),
                           result.end(),
                           result.begin(),
                           [](intrusive_ptr<DocumentSource> d) -> intrusive_ptr<DocumentSource> {
                               return (*d).optimize();
                           });
        }

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result.front().get());
        ASSERT(groupStage);

        const auto* sortStage = dynamic_cast<DocumentSourceSort*>(result.back().get());
        ASSERT(sortStage);

        // Serialize the DocumentSourceGroup and DocumentSourceSort from $bucket so that we can
        // check the explain output to make sure $group and $sort have the correct fields.
        auto explain = SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};
        vector<Value> explainedStages;
        groupStage->serializeToArray(explainedStages, explain);
        sortStage->serializeToArray(explainedStages, explain);
        ASSERT_EQUALS(explainedStages.size(), 2UL);

        auto groupExplain = explainedStages[0];
        ASSERT_VALUE_EQ(groupExplain["$group"], expectedGroupExplain);

        auto sortExplain = explainedStages[1];

        auto expectedSortExplain = Value{Document{{"sortKey", Document{{"_id", 1}}}}};
        ASSERT_VALUE_EQ(sortExplain["$sort"], expectedSortExplain);
    }
};

TEST_F(BucketReturnsGroupAndSort,
       BucketWithAllConstantsAndGroupByConstIsCorrectlyOptimizedAfterSwitch) {
    const auto spec = fromjson(
        "{$bucket : {groupBy : {$const : 6}, boundaries : [ 1, 5, 8 ], default : 'other'}}");

    auto expectedGroupWithOpt =
        Value(fromjson("{_id: {$const: 5}, count: {$sum: {$const: 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupWithOpt, true);
}

TEST_F(BucketReturnsGroupAndSort,
       BucketWithAllConstantFalsesAndGroupByConstIsCorrectlyOptimizedAfterSwitch) {
    const auto spec = fromjson(
        "{$bucket : {groupBy : {$const : 9}, boundaries : [ 1, 5, 8 ], default : 'other'}}");

    auto expectedGroupWithOpt = Value(
        fromjson("{_id: {$const: 'other'}, count: {$sum: {$const: 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupWithOpt, true);
}

TEST_F(BucketReturnsGroupAndSort,
       BucketWithAllConstantFalsesAndNoDefaultThrowsUassertErrorWhenOptimized) {
    const auto spec = fromjson("{$bucket : {groupBy : {$const : 9}, boundaries : [ 1, 5, 8 ]}}");

    list<intrusive_ptr<DocumentSource>> result =
        DocumentSourceBucket::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_EQUALS(result.size(), 2UL);

    list<intrusive_ptr<DocumentSource>> result_opt;
    result_opt.resize(result.size());

    ASSERT_THROWS_CODE(
        std::transform(result.begin(),
                       result.end(),
                       result_opt.begin(),
                       [](intrusive_ptr<DocumentSource> d) -> intrusive_ptr<DocumentSource> {
                           return (*d).optimize();
                       }),
        AssertionException,
        40069);
}

TEST_F(BucketReturnsGroupAndSort, BucketWithAllConstantsIsCorrectlyOptimizedAfterSwitch) {
    const auto spec = fromjson(
        "{$bucket : {groupBy : {$add: [2, 4]}, boundaries : [ 1, 5, 8 ], default : 'other'}}");

    auto expectedGroupWithOpt =
        Value(fromjson("{_id: {$const: 5}, count: {$sum: {$const: 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupWithOpt, true);
}

TEST_F(BucketReturnsGroupAndSort,
       BucketWithAllConstantsAndNoDefaultIsCorrectlyOptimizedAfterSwitch) {
    const auto spec = fromjson("{$bucket : {groupBy : {$add: [2, 4]}, boundaries : [ 1, 5, 8 ]}}");

    auto expectedGroupWithOpt =
        Value(fromjson("{_id: {$const: 5}, count: {$sum: {$const: 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupWithOpt, true);
}

TEST_F(BucketReturnsGroupAndSort, BucketUsesDefaultOutputWhenNoOutputSpecified) {
    const auto spec =
        fromjson("{$bucket : {groupBy :'$x', boundaries : [ 0, 2 ], default : 'other'}}");
    auto expectedGroupExplain = Value(
        fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : "
                 "0}]}, {$lt : ['$x', {$const : 2}]}]}, then : {$const : 0}}], default : "
                 "{$const : 'other'}}}, count : {$sum : {$const : 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenOutputSpecified) {
    const auto spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [0, 2], output : { number : {$sum : 1}}}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 2}]}]}, then : {$const : 0}}]}}, number : {$sum : {$const : 1}}, "
        "$willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenNoDefaultSpecified) {
    const auto spec = fromjson("{$bucket : { groupBy : '$x', boundaries : [0, 2]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 2}]}]}, then : {$const : 0}}]}}, count : {$sum : {$const : 1}}, "
        "$willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenBoundariesAreSameCanonicalType) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1.5]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 1.5}]}]}, then : {$const : 0}}]}},count : {$sum : {$const : 1}}, "
        "$willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenBoundariesAreConstantExpressions) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, {$add : [4, 5]}]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 9}]}]}, then : {$const : 0}}]}}, count : {$sum : {$const : 1}}, "
        "$willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenDefaultIsConstantExpression) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1], default: {$add : [4, 5]}}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const :"
                       "0}]}, {$lt : ['$x', {$const : 1}]}]}, then : {$const : 0}}], default : "
                       "{$const : 9}}}, count : {$sum : {$const : 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWithMultipleBoundaryValues) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2]}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : "
                       "0}]}, {$lt : ['$x', {$const : 1}]}]}, then : {$const : 0}}, {case : {$and "
                       ": [{$gte : ['$x', {$const : 1}]}, {$lt : ['$x', {$const : 2}]}]}, then : "
                       "{$const : 1}}]}}, count : {$sum : {$const : 1}}, $willBeMerged: false}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketWithEmptyGroupByStrDoesNotAccessPastEndOfString) {
    // Verify that {groupBy: ''} is rejected _without_ attempting to read past the end of the empty
    // string.
    const auto spec =
        fromjson("{$bucket : {groupBy : '', boundaries : [ 1, 5, 8 ], default : 'other'}}");

    // Under a debug build, this would previously fail if an empty str for groupBy led to access
    // past the end of the string, with pos() > size() in StringData::operator[].
    // Verify that this reaches the intended uassert, rejecting the empty string, _without_ first
    // trying to read past the end of the string.
    ASSERT_THROWS_CODE(DocumentSourceBucket::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       40202);
}

class InvalidBucketSpec : public AggregationContextFixture {
public:
    list<intrusive_ptr<DocumentSource>> createBucket(BSONObj bucketSpec) {
        auto sources = DocumentSourceBucket::createFromBson(bucketSpec.firstElement(), getExpCtx());
        return sources;
    }
};

TEST_F(InvalidBucketSpec, BucketFailsWithNonObject) {
    auto spec = fromjson("{$bucket : 1}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40201);

    spec = fromjson("{$bucket : 'test'}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40201);
}

TEST_F(InvalidBucketSpec, BucketFailsWithUnknownField) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2], unknown : 'field'}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40197);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNoGroupBy) {
    const auto spec = fromjson("{$bucket : {boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40198);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNoBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x'}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40198);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonExpressionGroupBy) {
    auto spec = fromjson("{$bucket : {groupBy : {test : 'obj'}, boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40202);

    spec = fromjson("{$bucket : {groupBy : 'test', boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40202);

    spec = fromjson("{$bucket : {groupBy : 1, boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40202);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonArrayBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : 'test'}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40200);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : 1}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40200);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : {test : 'obj'}}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40200);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNotEnoughBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40192);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : []}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40192);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonConstantValueBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : ['$x', '$y', '$z']}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40191);
}

TEST_F(InvalidBucketSpec, BucketFailsWithMixedTypesBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 'test']}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40193);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonUniqueBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 1, 2, 3]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40194);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : ['a', 'b', 'b', 'c']}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40194);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonSortedBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [4, 5, 3, 6]}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40194);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonConstantExpressionDefault) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2], default : '$x'}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40195);
}

TEST_F(InvalidBucketSpec, BucketFailsWhenDefaultIsInBoundariesRange) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 4], default : 3}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40199);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 4], default : 1}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40199);
}

TEST_F(InvalidBucketSpec, GroupFailsForBucketWithInvalidOutputField) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : 'test'}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40196);

    spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : {number : 'test'}}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40234);

    spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : {'test.test' : {$sum : "
        "1}}}}");
    ASSERT_THROWS_CODE(createBucket(spec), AssertionException, 40235);
}

TEST_F(InvalidBucketSpec, SwitchFailsForBucketWhenNoDefaultSpecified) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 3]}}");
    list<intrusive_ptr<DocumentSource>> bucketStages = createBucket(spec);

    ASSERT_EQUALS(bucketStages.size(), 2UL);

    auto groupStage = exec::agg::buildStage(bucketStages.front());
    ASSERT(groupStage);

    const auto* sortStage = dynamic_cast<DocumentSourceSort*>(bucketStages.back().get());
    ASSERT(sortStage);

    auto doc = Document{{"x", 4}};
    auto stage = exec::agg::MockStage::createForTest(doc, getExpCtx());

    groupStage->setSource(stage.get());
    ASSERT_THROWS_CODE(groupStage->getNext(), AssertionException, 40066);
}
}  // namespace
}  // namespace mongo

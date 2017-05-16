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
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_bucket.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::vector;
using boost::intrusive_ptr;

class BucketReturnsGroupAndSort : public AggregationContextFixture {
public:
    void testCreateFromBsonResult(BSONObj bucketSpec, Value expectedGroupExplain) {
        vector<intrusive_ptr<DocumentSource>> result =
            DocumentSourceBucket::createFromBson(bucketSpec.firstElement(), getExpCtx());

        ASSERT_EQUALS(result.size(), 2UL);

        const auto* groupStage = dynamic_cast<DocumentSourceGroup*>(result[0].get());
        ASSERT(groupStage);

        const auto* sortStage = dynamic_cast<DocumentSourceSort*>(result[1].get());
        ASSERT(sortStage);

        // Serialize the DocumentSourceGroup and DocumentSourceSort from $bucket so that we can
        // check the explain output to make sure $group and $sort have the correct fields.
        auto explain = ExplainOptions::Verbosity::kQueryPlanner;
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

TEST_F(BucketReturnsGroupAndSort, BucketUsesDefaultOutputWhenNoOutputSpecified) {
    const auto spec =
        fromjson("{$bucket : {groupBy :'$x', boundaries : [ 0, 2 ], default : 'other'}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : "
                       "0}]}, {$lt : ['$x', {$const : 2}]}]}, then : {$const : 0}}], default : "
                       "{$const : 'other'}}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenOutputSpecified) {
    const auto spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [0, 2], output : { number : {$sum : 1}}}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 2}]}]}, then : {$const : 0}}]}}, number : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenNoDefaultSpecified) {
    const auto spec = fromjson("{$bucket : { groupBy : '$x', boundaries : [0, 2]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 2}]}]}, then : {$const : 0}}]}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenBoundariesAreSameCanonicalType) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1.5]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 1.5}]}]}, then : {$const : 0}}]}},count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenBoundariesAreConstantExpressions) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, {$add : [4, 5]}]}}");
    auto expectedGroupExplain = Value(fromjson(
        "{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : 0}]}, {$lt : "
        "['$x', {$const : 9}]}]}, then : {$const : 0}}]}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWhenDefaultIsConstantExpression) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1], default: {$add : [4, 5]}}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const :"
                       "0}]}, {$lt : ['$x', {$const : 1}]}]}, then : {$const : 0}}], default : "
                       "{$const : 9}}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

TEST_F(BucketReturnsGroupAndSort, BucketSucceedsWithMultipleBoundaryValues) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2]}}");
    auto expectedGroupExplain =
        Value(fromjson("{_id : {$switch : {branches : [{case : {$and : [{$gte : ['$x', {$const : "
                       "0}]}, {$lt : ['$x', {$const : 1}]}]}, then : {$const : 0}}, {case : {$and "
                       ": [{$gte : ['$x', {$const : 1}]}, {$lt : ['$x', {$const : 2}]}]}, then : "
                       "{$const : 1}}]}}, count : {$sum : {$const : 1}}}"));

    testCreateFromBsonResult(spec, expectedGroupExplain);
}

class InvalidBucketSpec : public AggregationContextFixture {
public:
    vector<intrusive_ptr<DocumentSource>> createBucket(BSONObj bucketSpec) {
        auto sources = DocumentSourceBucket::createFromBson(bucketSpec.firstElement(), getExpCtx());
        return sources;
    }
};

TEST_F(InvalidBucketSpec, BucketFailsWithNonObject) {
    auto spec = fromjson("{$bucket : 1}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40201);

    spec = fromjson("{$bucket : 'test'}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40201);
}

TEST_F(InvalidBucketSpec, BucketFailsWithUnknownField) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2], unknown : 'field'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40197);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNoGroupBy) {
    const auto spec = fromjson("{$bucket : {boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40198);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNoBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40198);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonExpressionGroupBy) {
    auto spec = fromjson("{$bucket : {groupBy : {test : 'obj'}, boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40202);

    spec = fromjson("{$bucket : {groupBy : 'test', boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40202);

    spec = fromjson("{$bucket : {groupBy : 1, boundaries : [0, 1, 2]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40202);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonArrayBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : 'test'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40200);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : 1}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40200);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : {test : 'obj'}}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40200);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNotEnoughBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40192);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : []}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40192);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonConstantValueBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : ['$x', '$y', '$z']}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40191);
}

TEST_F(InvalidBucketSpec, BucketFailsWithMixedTypesBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 'test']}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40193);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonUniqueBoundaries) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 1, 2, 3]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40194);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : ['a', 'b', 'b', 'c']}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40194);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonSortedBoundaries) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [4, 5, 3, 6]}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40194);
}

TEST_F(InvalidBucketSpec, BucketFailsWithNonConstantExpressionDefault) {
    const auto spec =
        fromjson("{$bucket : {groupBy : '$x', boundaries : [0, 1, 2], default : '$x'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40195);
}

TEST_F(InvalidBucketSpec, BucketFailsWhenDefaultIsInBoundariesRange) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 4], default : 3}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40199);

    spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 4], default : 1}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40199);
}

TEST_F(InvalidBucketSpec, GroupFailsForBucketWithInvalidOutputField) {
    auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : 'test'}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40196);

    spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : {number : 'test'}}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40234);

    spec = fromjson(
        "{$bucket : {groupBy : '$x', boundaries : [1, 2, 3], output : {'test.test' : {$sum : "
        "1}}}}");
    ASSERT_THROWS_CODE(createBucket(spec), UserException, 40235);
}

TEST_F(InvalidBucketSpec, SwitchFailsForBucketWhenNoDefaultSpecified) {
    const auto spec = fromjson("{$bucket : {groupBy : '$x', boundaries : [1, 2, 3]}}");
    vector<intrusive_ptr<DocumentSource>> bucketStages = createBucket(spec);

    ASSERT_EQUALS(bucketStages.size(), 2UL);

    auto* groupStage = dynamic_cast<DocumentSourceGroup*>(bucketStages[0].get());
    ASSERT(groupStage);

    const auto* sortStage = dynamic_cast<DocumentSourceSort*>(bucketStages[1].get());
    ASSERT(sortStage);

    auto doc = Document{{"x", 4}};
    auto source = DocumentSourceMock::create(doc);
    groupStage->setSource(source.get());
    ASSERT_THROWS_CODE(groupStage->getNext(), UserException, 40066);
}
}  // namespace
}  // namespace mongo

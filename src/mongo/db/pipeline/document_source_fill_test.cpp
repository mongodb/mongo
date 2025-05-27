/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_fill.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceFillTest = AggregationContextFixture;

TEST_F(DocumentSourceFillTest, FillOnlyMethodsDesugarsCorrectly) {
    // Fill with value desugars as expected.
    auto fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("value" << 5))));
    auto stages = document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx());
    auto expectedStages = fromjson(R"({
         "$addFields": { "valToFill": { "$ifNull": [ "$valToFill", { "$const": 5 } ] } }
     })");
    // Expect this to be one stage only.
    ASSERT_EQ(stages.size(), 1);
    ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(), expectedStages.toString());

    // Fill with method desugars as expected. Note that linear fill requires a sort order, so we'll
    // test that separately.
    fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "locf"))));
    stages = document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx());
    expectedStages = fromjson(R"({
         "$_internalSetWindowFields": { "output": { "valToFill": { "$locf": "$valToFill" } } }
     })");
    ASSERT_EQ(stages.size(), 1);
    ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(), expectedStages.toString());
}

TEST_F(DocumentSourceFillTest, FillWithSortDesugarsCorrectly) {
    auto fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "linear"))
                                                  << "sortBy" << BSON("val" << 1)));
    auto stages = document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx());
    auto expectedStageOne = fromjson(R"({
         $sort: { sortKey: { val: 1 }, outputSortKeyMetadata: true }
     })");
    auto expectedStageTwo = fromjson(R"({
         $_internalSetWindowFields: { sortBy: { val: 1 }, output: { valToFill: { $linearFill: "$valToFill" } } }
     })");
    ASSERT_EQ(stages.size(), 2);
    ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(), expectedStageOne.toString());
    ASSERT_EQ(stages.back()->serializeToBSONForDebug().toString(), expectedStageTwo.toString());
}

TEST_F(DocumentSourceFillTest, FillWithPartitionsDesugarsCorrectly) {
    auto test = [&](const BSONObj& fillSpec, const std::string& expectedPartitionBy) {
        auto stages = document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx());
        auto expectedInclusion = fromjson(R"({
            $addFields: { __internal_setWindowFields_partition_key: )" +
                                          expectedPartitionBy + R"( }
            })");
        auto expectedSort = fromjson(R"({
            $sort: {
            sortKey: { __internal_setWindowFields_partition_key: 1, val: 1 },
            outputSortKeyMetadata: true
            }})");
        auto expectedSetWindowFields = fromjson(R"({
            $_internalSetWindowFields: {
            partitionBy: "$__internal_setWindowFields_partition_key",
            sortBy: { val: 1 },
            output: { valToFill: { $linearFill: "$valToFill" } }
            }})");
        auto expectedExclusion = fromjson(R"({
            $project: { __internal_setWindowFields_partition_key: false, _id: true }
            })");
        ASSERT_EQ(stages.size(), 4);
        ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(),
                  expectedInclusion.toString());
        stages.pop_front();
        ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(), expectedSort.toString());
        stages.pop_front();
        ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(),
                  expectedSetWindowFields.toString());
        stages.pop_front();
        ASSERT_EQ(stages.front()->serializeToBSONForDebug().toString(),
                  expectedExclusion.toString());
    };

    auto fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "linear"))
                                                  << "sortBy" << BSON("val" << 1) << "partitionBy"
                                                  << "$$ROOT"));
    test(fillSpec, "\"$$ROOT\"");

    fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "linear"))
                                             << "sortBy" << BSON("val" << 1) << "partitionBy"
                                             << BSON("part" << "$part")));
    test(fillSpec, R"({ $expr: { part: "$part" } })");
}

TEST_F(DocumentSourceFillTest, OptimizedOutParamsAreStillValidated) {
    // sortBy and partitionBy will be optimized out if output.foo.method is unused as
    // document_source_set_window_fields will not be used as a source. This means
    // it is up to document_source_fill to still validate these params if they are present.

    // Invalid sortBy with validation performed in document_source_fill.
    auto fillSpec =
        BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("value" << 1)) << "sortBy"
                                      << BSON("$val" << 1) << "partitionBy" << BSON("valid" << 1)));
    ASSERT_THROWS_CODE(document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx()),
                       AssertionException,
                       16410);

    // Invalid sortBy with validation performed in document_source_set_window_fields.
    fillSpec =
        BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "linear")) << "sortBy"
                                      << BSON("$val" << 1) << "partitionBy" << BSON("valid" << 1)));
    ASSERT_THROWS_CODE(document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx()),
                       AssertionException,
                       16410);

    // Invalid partitionBy with validation performed in document_source_fill.
    fillSpec =
        BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("value" << 1)) << "sortBy"
                                      << BSON("val" << 1) << "partitionBy" << BSON("$bogus" << 1)));
    ASSERT_THROWS_CODE(document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidPipelineOperator);

    // Invalid partitionBy with validation performed in document_source_set_window_fields.
    fillSpec =
        BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "locf")) << "sortBy"
                                      << BSON("val" << 1) << "partitionBy" << BSON("$bogus" << 1)));
    ASSERT_THROWS_CODE(document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::InvalidPipelineOperator);

    // Invalid partitionByField with validation performed in document_source_fill.
    fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("value" << 1)) << "sortBy"
                                             << BSON("val" << 1) << "partitionBy"
                                             << "$"));
    ASSERT_THROWS_CODE(document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx()),
                       AssertionException,
                       16872);

    // Invalid partitionByField with validated performed in document_source_set_window_fields.
    fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("method" << "locf"))
                                             << "sortBy" << BSON("val" << 1) << "partitionBy"
                                             << "$"));
    ASSERT_THROWS_CODE(document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx()),
                       AssertionException,
                       16872);

    // Fully validated expression should only have 1 stage when not using output.foo.method
    fillSpec = BSON("$fill" << BSON("output" << BSON("valToFill" << BSON("value" << 1)) << "sortBy"
                                             << BSON("val.foo" << 1) << "partitionBy"
                                             << BSON("valid" << "$valid")));
    auto stages = document_source_fill::createFromBson(fillSpec.firstElement(), getExpCtx());
    ASSERT_EQ(stages.size(), 1);
}

}  // namespace
}  // namespace mongo

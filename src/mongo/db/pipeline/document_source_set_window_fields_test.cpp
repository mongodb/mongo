/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSetWindowFieldsTest = AggregationContextFixture;

TEST_F(DocumentSourceSetWindowFieldsTest, FailsToParseInvalidArgumentTypes) {
    auto spec = BSON("$_internalSetWindowFields"
                     << "invalid");
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);

    spec = BSON("$_internalSetWindowFields" << BSON("sortBy"
                                                    << "invalid sort spec"));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);

    spec = BSON("$_internalSetWindowFields" << BSON("output"
                                                    << "invalid"));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);

    spec = BSON("$_internalSetWindowFields"
                << BSON("partitionBy" << BSON("$notAnExpression" << 1) << "output" << BSONObj()));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidPipelineOperator);

    spec = BSON("$_internalSetWindowFields" << BSON("unknown_parameter" << 1));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        40415);
}

TEST_F(DocumentSourceSetWindowFieldsTest, SuccessfullyParsesAndReserializes) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum: {$sum: 
        {input: '$pop', documents: [-10, 0]}}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    std::vector<Value> serializedArray;
    parsedStage->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(serializedArray[0].getDocument().toBson(), spec);
}

TEST_F(DocumentSourceSetWindowFieldsTest, FailsToParseIfFeatureFlagDisabled) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum: {$sum: 
        {input: '$pop', documents: [-10, 0]}}}}})");
    // By default, the unit test will have the feature flag disabled.
    ASSERT_THROWS_CODE(
        Pipeline::parse(std::vector<BSONObj>({spec}), getExpCtx()), AssertionException, 16436);
}

}  // namespace
}  // namespace mongo

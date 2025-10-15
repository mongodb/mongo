/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/api_parameters.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <climits>
#include <cmath>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace ExpressionTests {

class ExpressionMapReduceFilterTest : public mongo::unittest::Test {
public:
    ExpressionContextForTest& getExpCtx() {
        return *_expCtx;
    }

    // Parse 'json' into an expression of type T.
    template <class T>
    boost::intrusive_ptr<Expression> parse(StringData json) {
        return parse<T>(fromjson(json));
    }

    // Parse 'bson' into an expression of type T.
    template <class T>
    boost::intrusive_ptr<Expression> parse(BSONObj bson) {
        return T::parse(&getExpCtx(), bson.firstElement(), getExpCtx().variablesParseState);
    }

private:
    boost::optional<ExpressionContextForTest> _expCtx;

    void setUp() override {
        _expCtx.emplace();
    }

    void tearDown() override {
        _expCtx.reset();
    }
};

// Assert that EXPRESSION throws an exception with the expected error code.
#define ASSERT_CODE(EXPRESSION, EXPECTED_CODE) \
    ASSERT_THROWS_CODE(EXPRESSION, DBException, EXPECTED_CODE)

static Document fromJson(const std::string& json) {
    return Document(fromjson(json));
}

/* ------------------------- ExpressionMap -------------------------- */

TEST_F(ExpressionMapReduceFilterTest, MapNonArray) {
    auto expressionMap = parse<ExpressionMap>("{ $map: {input: 'MongoDB', in: 15213}}");
    ASSERT_CODE(expressionMap->evaluate(MutableDocument().freeze(), &getExpCtx().variables), 16883);
}

// Test several parsing errors.
TEST_F(ExpressionMapReduceFilterTest, MapParseConstraints) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);

    // Uppercase first letter.
    ASSERT_CODE(
        parse<ExpressionMap>("{ $map: {input: [1, 2, 3], arrayIndexAs: 'Idx', in: '$$Idx'}}"),
        ErrorCodes::FailedToParse);

    // Identifier starting with a dollar.
    ASSERT_CODE(parse<ExpressionMap>("{ $map: {input: [1, 2, 3], arrayIndexAs: '$i', in: '$$$i'}}"),
                ErrorCodes::FailedToParse);

    // Identifier starting with two dollars.
    ASSERT_CODE(
        parse<ExpressionMap>("{ $map: {input: [1, 2, 3], arrayIndexAs: '$$i', in: '$$$$i'}}"),
        ErrorCodes::FailedToParse);

    // Identifier starting with special characters.
    ASSERT_CODE(
        parse<ExpressionMap>("{ $map: {input: [1, 2, 3], arrayIndexAs: '\\\\a', in: '$$\\\\a'}}"),
        ErrorCodes::FailedToParse);

    ASSERT_CODE(parse<ExpressionMap>("{ $map: {input: [1, 2, 3], arrayIndexAs: '*a', in: '$$*a'}}"),
                ErrorCodes::FailedToParse);

    ASSERT_CODE(parse<ExpressionMap>("{ $map: {input: [1, 2, 3], arrayIndexAs: '_a', in: '$$_a'}}"),
                ErrorCodes::FailedToParse);

    // Identifier with embedded null.
    StringData str("fo\0o", 4);
    BSONObj query = BSONObjBuilder()
                        .append("$map",
                                BSONObjBuilder()
                                    .appendArray("input", BSON_ARRAY(1 << 2 << 3))
                                    .append("arrayIndexAs", str)
                                    .append("in", str)
                                    .obj())
                        .obj();
    ASSERT_CODE(parse<ExpressionMap>(query), ErrorCodes::FailedToParse);
}

TEST(ExpressionMapTest, MapToConstant) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $map: { input: { $literal: [1, 2, 3]}, in: 1 } }");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 1 << 1)));
}

TEST(ExpressionMapTest, MapAddOne) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $map: { input: { $literal: [3, 1, 2]}, as: 'v', in: { $add: ['$$v', 1]} } }");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(4 << 2 << 3)));
}

TEST(ExpressionMapTest, MapDivideZero) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $map: { input: { $literal: [3, 2, 1, 0]}, as: 'v', in: { $divide: [6, '$$v']} } }");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(ExpressionMapTest, MapEmptyWithExceptionInit) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $map: { input: { $literal: []}, as: 'v', in: { $divide: [15445, 0]} } }");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONArray()));
}

TEST(ExpressionMapTest, MapTypeMismatch) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $map: { input: { $literal: [1, 2, 3]}, as: 'v', in: { $concat: ['$$v', 'MongoDB']} } }");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(
        expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables), DBException, 16702);
}

TEST(ExpressionMapTest, MapIndices) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $map: { input: { $literal: [1, 1, 1]}, in: '$$IDX'}}");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(0 << 1 << 2)));
}

TEST(ExpressionMapTest, MapIndicesNamed) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $map: { input: { $literal: [1, 1, 1]}, arrayIndexAs: 'i', in: '$$i'}}");

    auto expressionMap =
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionMap->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(0 << 1 << 2)));
}

TEST(ExpressionMapTest, MapIndicesNamedFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $map: { input: { $literal: [1, 1, 1]}, arrayIndexAs: 'i', in: '$$i'}}");

    ASSERT_THROWS_CODE(
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        DBException,
        16879);
}

TEST(ExpressionMapTest, MapIndicesDefaultFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $map: { input: { $literal: [1, 1, 1]}, in: '$$IDX'}}");

    ASSERT_THROWS_CODE(
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        DBException,
        17276);
}

TEST(ExpressionMapTest, MapIndicesAPIStrict) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIVersion("1");
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    BSONObj expr =
        fromjson("{ $map: { input: { $literal: [1, 1, 1]}, arrayIndexAs: 'i', in: '$$i'}}");

    ASSERT_THROWS_CODE(
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        AssertionException,
        ErrorCodes::APIStrictError);
}

TEST(ExpressionMapTest, MapIndicesAPIStrictFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIVersion("1");
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    BSONObj expr =
        fromjson("{ $map: { input: { $literal: [1, 1, 1]}, arrayIndexAs: 'i', in: '$$i'}}");

    ASSERT_THROWS_CODE(
        ExpressionMap::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        AssertionException,
        16879);
}

/* ------------------------- ExpressionReduce -------------------------- */

// Test several parsing errors.
TEST_F(ExpressionMapReduceFilterTest, ReduceParseConstraints) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);

    // Identifier with uppercase first letter.
    ASSERT_CODE(parse<ExpressionReduce>("{ $reduce: { input: [1, 2, 3], initialValue: 0, "
                                        "arrayIndexAs: 'Idx', in: { $add: ['$$Idx', 1]}}}"),
                ErrorCodes::FailedToParse);

    // Identifier starting with a dollar.
    ASSERT_CODE(parse<ExpressionReduce>("{ $reduce: { input: [1, 2, 3], initialValue: 0, "
                                        "arrayIndexAs: '$i', in: { $add: ['$$$i', 1]}}}"),
                ErrorCodes::FailedToParse);

    // Identifier starting with two dollars.
    ASSERT_CODE(parse<ExpressionReduce>("{ $reduce: { input: [1, 2, 3], initialValue: 0, "
                                        "arrayIndexAs: '$$i', in: { $add: ['$$$$i', 1]}}}"),
                ErrorCodes::FailedToParse);

    // Identifier starting with special characters.
    ASSERT_CODE(parse<ExpressionReduce>("{ $reduce: { input: [1, 2, 3], initialValue: 0, "
                                        "arrayIndexAs: '\\\\a', in: { $add: ['$$\\\\a', 1]}}}"),
                ErrorCodes::FailedToParse);

    ASSERT_CODE(parse<ExpressionReduce>("{ $reduce: { input: [1, 2, 3], initialValue: 0, "
                                        "arrayIndexAs: '*a', in: { $add: ['$$*a', 1]}}}"),
                ErrorCodes::FailedToParse);

    ASSERT_CODE(parse<ExpressionReduce>("{ $reduce: { input: [1, 2, 3], initialValue: 0, "
                                        "arrayIndexAs: '_a', in: { $add: ['$$_a', 1]}}}"),
                ErrorCodes::FailedToParse);

    // Identifier with embedded null.
    StringData str("fo\0o", 4);
    BSONObj query =
        BSONObjBuilder()
            .append(
                "$reduce",
                BSONObjBuilder()
                    .appendArray("input", BSON_ARRAY(1 << 2 << 3))
                    .append("initialValue", 0)
                    .append("arrayIndexAs", str)
                    .append("in", BSONObjBuilder().appendArray("$add", BSON_ARRAY(str << 1)).obj())
                    .obj())
            .obj();
    ASSERT_CODE(parse<ExpressionReduce>(query), ErrorCodes::FailedToParse);
}

TEST(ExpressionReduceTest, ReduceNonArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: 'MongoDB', initialValue: 15213, in: { $add: ['$$value', 1]}}}");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables),
                       DBException,
                       40080);
}

TEST(ExpressionReduceTest, ReduceEmptyArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: { $literal: [] }, initialValue: 15150, in: { $add: [ '$$value', "
        "'$$this' ]"
        "} } }");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(15150));
}

TEST(ExpressionReduceTest, ReduceStringConcat) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: { $literal: ['a', 'b', 'c']}, initialValue: '', in: {$concat: "
        "['$$value', '$$this']} }}");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(("abc"_sd)));
}

TEST(ExpressionReduceTest, ReduceSumProduct) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: {$literal: [1, 2, 3, 4, 5]}, initialValue: { sum: 5, product: 2 }, "
        "in: {sum : {$add: ['$$this', '$$value.sum']}, product: {$multiply: ['$$this', "
        "'$$value.product']}} } "
        "}");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables);

    BSONObj res = fromjson("{sum: 20, product: 240}");
    ASSERT_VALUE_EQ(val, Value(res));
}

TEST(ExpressionReduceTest, ReduceEmptyExceptionInitialValue) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: { $literal: []}, initialValue: {$divide: [48, 0]}, in: {$divide: "
        "['$$this', '$$value']} } }");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(ExpressionReduceTest, ReduceEmptyExceptionExpression) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: { $literal: []}, initialValue: 15312, in: {$divide: ['$$this', 0]}} "
        "}");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(15312));
}

TEST(ExpressionReduceTest, ReduceIndicesDefault) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: { $literal: [1, 1, 1]}, initialValue: 0, in: "
        "{$add: ['$$this', "
        "'$$value', '$$IDX']}}}");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(6));
}

TEST(ExpressionReduceTest, ReduceIndicesNamed) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $reduce: { input: { $literal: [1, 1, 1]}, initialValue: 0, arrayIndexAs: 'i', in: "
        "{$add: ['$$this', "
        "'$$value', '$$i']}}}");

    auto expressionReduce =
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionReduce->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(6));
}

TEST(ExpressionReduceTest, ReduceIndicesAPIStrict) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIVersion("1");
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    BSONObj expr = fromjson(
        "{ $reduce: {input: [1, 2, 3], arrayIndexAs: 'i', in: {$add: ['$$this', '$$value', '$$i']} "
        "}}");

    ASSERT_THROWS_CODE(
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        AssertionException,
        ErrorCodes::APIStrictError);
}

TEST(ExpressionReduceTest, ReduceIndicesAPIStrictFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIVersion("1");
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    BSONObj expr = fromjson(
        "{ $reduce: {input: [1, 2, 3], arrayIndexAs: 'i', in: {$add: ['$$this', '$$value', '$$i']} "
        "}}");

    ASSERT_THROWS_CODE(
        ExpressionReduce::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        AssertionException,
        40076);
}

/* ------------------------- ExpressionFilter -------------------------- */

// Test several parsing errors.
TEST_F(ExpressionMapReduceFilterTest, FilterParseConstraints) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);

    // Identifier with uppercase first letter.
    ASSERT_CODE(
        parse<ExpressionFilter>(
            "{ $filter: { input: [1, 2, 3], arrayIndexAs: 'Idx', cond: { $lte: [ '$$Idx', 2]}}}"),
        ErrorCodes::FailedToParse);

    // Identifier starting with a dollar.
    ASSERT_CODE(
        parse<ExpressionFilter>(
            "{ $filter: { input: [1, 2, 3], arrayIndexAs: '$i', cond: { $lte: [ '$$$i', 2]}}}"),
        ErrorCodes::FailedToParse);

    // Identifier starting with two dollars.
    ASSERT_CODE(
        parse<ExpressionFilter>(
            "{ $filter: { input: [1, 2, 3], arrayIndexAs: '$$i', cond: { $lte: [ '$$$$i', 2]}}}"),
        ErrorCodes::FailedToParse);

    // Identifier starting with special characters.
    ASSERT_CODE(parse<ExpressionFilter>("{ $filter: { input: [1, 2, 3], arrayIndexAs: '\\\\a', "
                                        "cond: { $lte: [ '$$\\\\a', 2]}}}"),
                ErrorCodes::FailedToParse);

    ASSERT_CODE(
        parse<ExpressionFilter>(
            "{ $filter: { input: [1, 2, 3], arrayIndexAs: '*a', cond: { $lte: [ '$$*a', 2]}}}"),
        ErrorCodes::FailedToParse);

    ASSERT_CODE(
        parse<ExpressionFilter>(
            "{ $filter: { input: [1, 2, 3], arrayIndexAs: '_a', cond: { $lte: [ '$$_a', 2]}}}"),
        ErrorCodes::FailedToParse);

    // Identifier with embedded null.
    StringData str("fo\0o", 4);
    BSONObj query =
        BSONObjBuilder()
            .append("$filter",
                    BSONObjBuilder()
                        .appendArray("input", BSON_ARRAY(1 << 2 << 3))
                        .append("arrayIndexAs", str)
                        .append("cond",
                                BSONObjBuilder().appendArray("$lte", BSON_ARRAY(str << 2)).obj())
                        .obj())
            .obj();
    ASSERT_CODE(parse<ExpressionFilter>(query), ErrorCodes::FailedToParse);
}

TEST(ExpressionFilterTest, FilterNonArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $filter: { input: 'MongoDB', as: 'v', cond: true}}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables),
                       DBException,
                       28651);
}

TEST(ExpressionFilterTest, FilterInvalidLimit) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: {$literal: [1, 2, 3]}, as: 'v', cond: { $lte: [ '$$v', 2]}, limit: -1 "
        "}}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables),
                       DBException,
                       327392);
}

TEST(ExpressionFilterTest, FilterTypeMismatchLimit) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: {$literal: [1, 2, 3]}, as: 'v', cond: { $lte: [ '$$v', 2]}, limit: "
        "'Functions are Values' "
        "}}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    ASSERT_THROWS_CODE(expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables),
                       DBException,
                       327391);
}

TEST(ExpressionFilterTest, FilterExtraLimit) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $filter: { input: {$literal: [1, 2, 3]}, as: 'v', cond: true , limit: 5 }}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}

TEST(ExpressionFilterTest, FilterNullLimit) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $filter: { input: {$literal: [1, 2, 3]}, as: 'v', cond: true , limit: null }}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}

TEST(ExpressionFilterTest, FilterIntegerComparison) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [1, 2, 3, 4, 5]}, as: 'v', cond: { $lte: [ '$$v', 3 ] } } "
        "}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}

TEST(ExpressionFilterTest, FilterIsNumber) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [1, 'a', 2, null, 3.1, NumberLong(4), '5']}, as: 'v', "
        "cond: { $isNumber: '$$v'}, limit: 3 } }");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3.1)));
}

TEST(ExpressionFilterTest, FilterStringEqualityOnField) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [ {city: 'Pittsburgh', population: 302898}, {city: 'NYC', "
        "population: 8336000}]}, as: 'v', cond: { $eq: ['$$v.city', 'Pittsburgh']} } }");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(fromjson("{city: 'Pittsburgh', population: 302898}"))));
}

TEST(ExpressionFilterTest, FilterEmptyWithExceptionCondition) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $filter: { input: { $literal: []}, as: 'v', cond: { $divide: [1, 0]}} }");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONArray()));
}

TEST(ExpressionFilterTest, FilterConditionFalse) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 'c', NumberLong(12), true]}, as: 'v', cond: false} }");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONArray()));
}

TEST(ExpressionFilterTest, FilterIndicesDefault) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', cond: { $eq: ['$$v', "
        "'$$IDX']}} }");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(0 << 2 << 4)));
}

TEST(ExpressionFilterTest, FilterIndicesNamed) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', arrayIndexAs: 'i', cond: { "
        "$eq: ['$$v', "
        "'$$i']}} }");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(0 << 2 << 4)));
}

TEST(ExpressionFilterTest, FilterIndicesDefaultLimited) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', cond: { $eq: ['$$v', "
        "'$$IDX']}, limit: 2 }}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(0 << 2)));
}

TEST(ExpressionFilterTest, FilterIndicesNamedLimited) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', arrayIndexAs: 'i', cond: { "
        "$eq: ['$$v', "
        "'$$i']}, limit: 1 }}");

    auto expressionFilter =
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionFilter->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(0)));
}

TEST(ExpressionFilterTest, FilterIndicesDefaultFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', cond: { $eq: ['$$v', "
        "'$$IDX']}} }");

    ASSERT_THROWS_CODE(
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        DBException,
        17276);
}

TEST(ExpressionFilterTest, FilterIndicesNamedFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', arrayIndexAs: 'i', cond: { "
        "$eq: ['$$v', "
        "'$$i']}} }");

    ASSERT_THROWS_CODE(
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        DBException,
        28647);
}

TEST(ExpressionFilterTest, FilterIndicesAPIStrict) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", true);
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIVersion("1");
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', arrayIndexAs: 'i', cond: { "
        "$eq: ['$$v', "
        "'$$i']}} }");

    ASSERT_THROWS_CODE(
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        AssertionException,
        ErrorCodes::APIStrictError);
}

TEST(ExpressionFilterTest, FilterIndicesAPIStrictFeatureDisabled) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagExposeArrayIndexInMapFilterReduce", false);
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIVersion("1");
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    BSONObj expr = fromjson(
        "{ $filter: { input: { $literal: [0, 4, 2, 7, 4, 0] }, as: 'v', arrayIndexAs: 'i', cond: { "
        "$eq: ['$$v', "
        "'$$i']}} }");

    ASSERT_THROWS_CODE(
        ExpressionFilter::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState),
        AssertionException,
        28647);
}

}  // namespace ExpressionTests
}  // namespace mongo

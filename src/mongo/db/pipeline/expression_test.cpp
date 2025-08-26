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

#include <absl/container/node_hash_map.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/api_parameters.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/name_expression.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/record_id.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/summation.h"
#include "mongo/util/time_support.h"

#include <climits>
#include <cmath>
#include <limits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace ExpressionTests {

using boost::intrusive_ptr;
using std::string;
using std::vector;

/** Convert BSONObj to a BSONObj with our $const wrappings. */
static BSONObj constify(const BSONObj& obj, bool parentIsArray = false) {
    BSONObjBuilder bob;
    for (BSONObjIterator itr(obj); itr.more(); itr.next()) {
        BSONElement elem = *itr;
        if (elem.type() == BSONType::object) {
            bob << elem.fieldName() << constify(elem.Obj(), false);
        } else if (elem.type() == BSONType::array && !parentIsArray) {
            // arrays within arrays are treated as constant values by the real
            // parser
            bob << elem.fieldName() << BSONArray(constify(elem.Obj(), true));
        } else if (elem.fieldNameStringData() == "$const" ||
                   (elem.type() == BSONType::string &&
                    elem.valueStringDataSafe().starts_with("$"))) {
            bob.append(elem);
        } else {
            bob.append(elem.fieldName(), BSON("$const" << elem));
        }
    }
    return bob.obj();
}

/** Convert Value to a wrapped BSONObj with an empty string field name. */
static BSONObj toBson(const Value& value) {
    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

/** Convert Expression to BSON. */
static BSONObj expressionToBson(const intrusive_ptr<Expression>& expression) {
    return BSON("" << expression->serialize()).firstElement().embeddedObject().getOwned();
}

/** Convert Document to BSON. */
static BSONObj toBson(const Document& document) {
    return document.toBson();
}

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj) {
    return Document(obj);
}

Document fromJson(const std::string& json) {
    return Document(fromjson(json));
}

/** Create a Value from a BSONObj. */
Value valueFromBson(BSONObj obj) {
    BSONElement element = obj.firstElement();
    return Value(element);
}

/* ------------------------- Old-style tests -------------------------- */
namespace Constant {

/** No optimization is performed. */
class Optimize {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
        // An attempt to optimize returns the Expression itself.
        ASSERT_EQUALS(expression, expression->optimize());
    }
};

/** No dependencies. */
class Dependencies {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
        DepsTracker dependencies;
        expression::addDependencies(expression.get(), &dependencies);
        ASSERT_EQUALS(0U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedsAnyMetadata());
    }
};

/** Output to BSONObj. */
class AddToBsonObj {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
        // The constant is replaced with a $ expression.
        ASSERT_BSONOBJ_BINARY_EQ(BSON("field" << BSON("$const" << 5)), toBsonObj(expression));
    }

private:
    static BSONObj toBsonObj(const intrusive_ptr<Expression>& expression) {
        return BSON("field" << expression->serialize());
    }
};

/** Output to BSONArray. */
class AddToBsonArray {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
        // The constant is copied out as is.
        ASSERT_BSONOBJ_BINARY_EQ(constify(BSON_ARRAY(5)), toBsonArray(expression));
    }

private:
    static BSONObj toBsonArray(const intrusive_ptr<Expression>& expression) {
        BSONArrayBuilder bab;
        bab << expression->serialize();
        return bab.obj();
    }
};

TEST(ExpressionConstantTest, ConstantOfValueMissingSerializesToRemoveSystemVar) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value());
    ASSERT_BSONOBJ_BINARY_EQ(BSON("field" << "$$REMOVE"), BSON("field" << expression->serialize()));
}

TEST(ExpressionConstantTest, ConstantRedaction) {
    SerializationOptions options;
    options.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;

    // Test that a constant is replaced.
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value("my_ssn"_sd));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":"?string"})",
        BSON("field" << expression->serialize(options)));

    auto expressionBSON = BSON("$and" << BSON_ARRAY(BSON("$gt" << BSON_ARRAY("$foo" << 5))
                                                    << BSON("$lt" << BSON_ARRAY("$foo" << 10))));
    expression = Expression::parseExpression(&expCtx, expressionBSON, expCtx.variablesParseState);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":{"$and":[{"$gt":["$foo","?number"]},{"$lt":["$foo","?number"]}]}})",
        BSON("field" << expression->serialize(options)));
}

}  // namespace Constant

TEST(ExpressionArray, ExpressionArrayWithAllConstantValuesShouldOptimizeToExpressionConstant) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // ExpressionArray of constant values should optimize to ExpressionConsant.
    BSONObj bsonarrayOfConstants = BSON("" << BSON_ARRAY(1 << 2 << 3 << 4));
    BSONElement elementArray = bsonarrayOfConstants.firstElement();
    auto expressionArr = ExpressionArray::parse(&expCtx, elementArray, vps);
    auto optimizedToConstant = expressionArr->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedToConstant.get());
    ASSERT_TRUE(exprConstant);

    // ExpressionArray with not all constant values should not optimize to ExpressionConstant.
    BSONObj bsonarray = BSON("" << BSON_ARRAY(1 << "$x" << 3 << 4));
    BSONElement elementArrayNotConstant = bsonarray.firstElement();
    auto expressionArrNotConstant = ExpressionArray::parse(&expCtx, elementArrayNotConstant, vps);
    auto notOptimized = expressionArrNotConstant->optimize();
    auto notExprConstant = dynamic_cast<ExpressionConstant*>(notOptimized.get());
    ASSERT_FALSE(notExprConstant);
}

TEST(ExpressionSwitch, ExpressionSwitchShouldFilterOutConstantFalsesWhenOptimized) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: 1}, {case: { $const: false}, then: 2}, {case: "
        "\"$y\", then: 3}], default: 4}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedRemovedConstF = switchExp->optimize();

    auto notExprConstant = dynamic_cast<ExpressionConstant*>(optimizedRemovedConstF.get());
    ASSERT_FALSE(notExprConstant);

    BSONObj switchOptResult = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: { $const: 1 }}, {case: \"$y\", then: { $const: "
        "3 }}], default: { $const: 4 }}}");
    ASSERT_BSONOBJ_BINARY_EQ(switchOptResult, expressionToBson(optimizedRemovedConstF));
}

TEST(ExpressionSwitch, ExpressionSwitchShouldFilterOutMultipleConstantFalsesWhenOptimized) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: { $const: false}, then: 5}, {case: \"$x\", then: 1}, {case: "
        "{ $const: false}, then: 2}, {case: { $const: false}, then: 2}, {case: \"$y\", then: 3}, "
        "{case: { $const: false}, then: 2}], default: 4}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedRemovedConstF = switchExp->optimize();

    auto notExprConstant = dynamic_cast<ExpressionConstant*>(optimizedRemovedConstF.get());
    ASSERT_FALSE(notExprConstant);

    BSONObj switchOptResult = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: { $const: 1 }}, {case: \"$y\", then: { $const: "
        "3 }}], default: { $const: 4 }}}");
    ASSERT_BSONOBJ_BINARY_EQ(switchOptResult, expressionToBson(optimizedRemovedConstF));
}

TEST(ExpressionSwitch, ExpressionSwitchWithAllConstantFalsesAndNoDefaultErrors) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: { $const: false}, then: 5}, {case: { $const: false}, then: "
        "1}, {case: "
        "{ $const: false}, then: 2}]}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    ASSERT_THROWS_CODE(switchExp->optimize(), AssertionException, 40069);
}

TEST(ExpressionSwitch, ExpressionSwitchWithZeroAsConstantFalseAndNoDefaultErrors) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: { $const: 0}, then: 5}, {case: { $const: false}, then: 1}, "
        "{case: "
        "{ $const: false}, then: 2}]}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    ASSERT_THROWS_CODE(switchExp->optimize(), AssertionException, 40069);
}

TEST(ExpressionSwitch, ExpressionSwitchShouldMakeConstTrueDefaultAndRemoveRest) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: 1}, {case: { $const: true}, then: 2}, {case: "
        "\"$y\", then: 3}], default: 4}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedRemovedConstT = switchExp->optimize();

    auto notExprConstant = dynamic_cast<ExpressionConstant*>(optimizedRemovedConstT.get());
    ASSERT_FALSE(notExprConstant);

    BSONObj switchOptResult = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: { $const: 1 }}], default: { $const: 2 }}}");
    ASSERT_BSONOBJ_BINARY_EQ(switchOptResult, expressionToBson(optimizedRemovedConstT));
}

TEST(ExpressionSwitch, ExpressionSwitchShouldOptimizeThensCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: {$add: [2, 4]}}, {case: { $const: true}, then: "
        "{$add: [3, 4]}}, {case: "
        "\"$y\", then: 3}], default: 4}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedRemovedConstT = switchExp->optimize();

    auto notExprConstant = dynamic_cast<ExpressionConstant*>(optimizedRemovedConstT.get());
    ASSERT_FALSE(notExprConstant);

    BSONObj switchOptResult = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: { $const: 6 }}], default: { $const: 7 }}}");
    ASSERT_BSONOBJ_BINARY_EQ(switchOptResult, expressionToBson(optimizedRemovedConstT));
}

TEST(ExpressionSwitch, ExpressionSwitchWithFirstCaseTrueShouldReturnFirstThenExpression) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: { $const: true}, then: 3}, {case: "
        "\"$y\", then: 4}], default: 4}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedRemovedConstT = switchExp->optimize();

    BSONObj switchOptResult = fromjson("{ $const: 3 }");
    ASSERT_BSONOBJ_BINARY_EQ(switchOptResult, expressionToBson(optimizedRemovedConstT));
}

TEST(ExpressionSwitch, ExpressionSwitchWithNoDefaultShouldMakeConstTrueDefaultAndRemoveRest) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: 1}, {case: { $const: true}, then: 2}, {case: "
        "\"$y\", then: 3}]}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedRemovedConstT = switchExp->optimize();

    auto notExprConstant = dynamic_cast<ExpressionConstant*>(optimizedRemovedConstT.get());
    ASSERT_FALSE(notExprConstant);

    BSONObj switchOptResult = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: { $const: 1 }}], default: { $const: 2 }}}");
    ASSERT_BSONOBJ_BINARY_EQ(switchOptResult, expressionToBson(optimizedRemovedConstT));
}

TEST(ExpressionSwitch, ExpressionSwitchWithNoCasesShouldReturnDefault) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: { $const: false}, then: 1}, {case: { $const: false}, then: "
        "2}], default: 4}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedDefault = switchExp->optimize();

    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedDefault.get());
    ASSERT_TRUE(exprConstant);
    ASSERT_VALUE_EQ(exprConstant->getValue(), Value(4));
}

TEST(ExpressionSwitch, ExpressionSwitchWithNoConstantsShouldStayTheSame) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(
        "{$switch: {branches: [{case: \"$x\", then: { $const: 1 }}, {case: \"$z\", then: { $const: "
        "2 }}, {case: \"$y\", then: { $const: 3 }}], default: { $const: 4 }}}");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedStaySame = switchExp->optimize();

    auto notExprConstant = dynamic_cast<ExpressionConstant*>(optimizedStaySame.get());
    ASSERT_FALSE(notExprConstant);

    ASSERT_BSONOBJ_BINARY_EQ(switchQ, expressionToBson(optimizedStaySame));
}

// This test was designed to provide coverage for SERVER-70190, a bug in which optimizing a $switch
// expression could leave its children vector in a bad state. By walking the tree after optimizing
// we make sure that the expected children are found.
TEST(ExpressionSwitch, CaseEliminationShouldLeaveTreeInWalkableState) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj switchQ = fromjson(R"(
        {$switch: {
            branches: [
                {case: false, then: {$const: 0}},
                {case: "$z", then: {$const: 1}},
                {case: "$y", then: {$const: 3}},
                {case: true, then: {$const: 4}},
                {case: "$a", then: {$const: 5}},
                {case: "$b", then: {$const: 6}},
                {case: "$c", then: {$const: 7}}
            ],
            default: {$const: 8}
        }}
    )");
    auto switchExp = ExpressionSwitch::parse(&expCtx, switchQ.firstElement(), vps);
    auto optimizedExpr = switchExp->optimize();

    BSONObj optimizedQ = fromjson(R"(
        {$switch: {
            branches: [
                {case: "$z", then: {$const: 1}},
                {case: "$y", then: {$const: 3}}
            ],
            default: {$const: 4}
        }}
    )");

    ASSERT_BSONOBJ_BINARY_EQ(optimizedQ, expressionToBson(optimizedExpr));

    // Make sure that the expression tree appears as expected when the children are traversed using
    // a for-each loop.
    int childNum = 0;
    int numConstants = 0;
    for (auto&& child : optimizedExpr->getChildren()) {
        // Children 0 and 2 are field path expressions, whereas 1, 3, and 4 are constants.
        auto constExpr = dynamic_cast<ExpressionConstant*>(child.get());
        if (constExpr) {
            ASSERT_VALUE_EQ(constExpr->getValue(), Value{childNum});
            ++numConstants;
        } else {
            ASSERT(dynamic_cast<ExpressionFieldPath*>(child.get()));
        }
        ++childNum;
    }
    // We should have seen 5 children total, 3 of which are constants.
    ASSERT_EQ(childNum, 5);
    ASSERT_EQ(numConstants, 3);
}

TEST(ExpressionArray, ExpressionArrayShouldOptimizeSubExpressionToExpressionConstant) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;


    // ExpressionArray with constant values and sub expression that evaluates to constant should
    // optimize to Expression constant.
    BSONObj bsonarrayWithSubExpression =
        BSON("" << BSON_ARRAY(1 << BSON("$add" << BSON_ARRAY(1 << 1)) << 3 << 4));
    BSONElement elementArrayWithSubExpression = bsonarrayWithSubExpression.firstElement();
    auto expressionArrWithSubExpression =
        ExpressionArray::parse(&expCtx, elementArrayWithSubExpression, vps);
    auto optimizedToConstantWithSubExpression = expressionArrWithSubExpression->optimize();
    auto constantExpression =
        dynamic_cast<ExpressionConstant*>(optimizedToConstantWithSubExpression.get());
    ASSERT_TRUE(constantExpression);
}

TEST(ExpressionIndexOfArray, ExpressionIndexOfArrayShouldOptimizeArguments) {
    auto expCtx = ExpressionContextForTest{};

    auto expIndexOfArray = Expression::parseExpression(
        &expCtx,  // 2, 1, 1
        BSON("$indexOfArray" << BSON_ARRAY(
                 BSON_ARRAY(BSON("$add" << BSON_ARRAY(1 << 1)) << 1 << 1 << 2)
                 // Value we are searching for = 2.
                 << BSON("$add" << BSON_ARRAY(1 << 1))
                 // Start index = 1.
                 << BSON("$add" << BSON_ARRAY(0 << 1))
                 // End index = 4.
                 << BSON("$add" << BSON_ARRAY(1 << 3)))),
        expCtx.variablesParseState);
    auto argsOptimizedToConstants = expIndexOfArray->optimize();
    auto shouldBeIndexOfArray = dynamic_cast<ExpressionConstant*>(argsOptimizedToConstants.get());
    ASSERT_TRUE(shouldBeIndexOfArray);
    ASSERT_VALUE_EQ(Value(3), shouldBeIndexOfArray->getValue());
}

TEST(ExpressionIndexOfArray,
     ExpressionIndexOfArrayShouldOptimizeNullishInputArrayToExpressionConstant) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    auto expIndex = Expression::parseExpression(
        &expCtx, fromjson("{ $indexOfArray : [ undefined , 1, 1]}"), expCtx.variablesParseState);

    auto isExpIndexOfArray = dynamic_cast<ExpressionIndexOfArray*>(expIndex.get());
    ASSERT_TRUE(isExpIndexOfArray);

    auto nullishValueOptimizedToExpConstant = isExpIndexOfArray->optimize();
    auto shouldBeExpressionConstant =
        dynamic_cast<ExpressionConstant*>(nullishValueOptimizedToExpConstant.get());
    ASSERT_TRUE(shouldBeExpressionConstant);
    // Nullish input array should become a Value(BSONNULL).
    ASSERT_VALUE_EQ(Value(BSONNULL), shouldBeExpressionConstant->getValue());
}

TEST(ExpressionIndexOfArray,
     OptimizedExpressionIndexOfArrayWithConstantArgumentsShouldEvaluateProperly) {

    auto expCtx = ExpressionContextForTest{};

    auto expIndexOfArray = Expression::parseExpression(
        &expCtx,
        // Search for $x.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 3, 4, 5, 'val'] , '$x'] }"),
        expCtx.variablesParseState);
    auto optimizedIndexOfArray = expIndexOfArray->optimize();
    ASSERT_VALUE_EQ(Value(0),
                    optimizedIndexOfArray->evaluate(Document{{"x", 0}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(1),
                    optimizedIndexOfArray->evaluate(Document{{"x", 1}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(2),
                    optimizedIndexOfArray->evaluate(Document{{"x", 2}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(3),
                    optimizedIndexOfArray->evaluate(Document{{"x", 3}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(4),
                    optimizedIndexOfArray->evaluate(Document{{"x", 4}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(5),
                    optimizedIndexOfArray->evaluate(Document{{"x", 5}}, &expCtx.variables));
    ASSERT_VALUE_EQ(
        Value(6),
        optimizedIndexOfArray->evaluate(Document{{"x", string("val")}}, &expCtx.variables));

    auto optimizedIndexNotFound = optimizedIndexOfArray->optimize();
    // Should evaluate to -1 if not found.
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", 10}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", 100}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", 1000}}, &expCtx.variables));
    ASSERT_VALUE_EQ(
        Value(-1),
        optimizedIndexNotFound->evaluate(Document{{"x", string("string")}}, &expCtx.variables));
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", -1}}, &expCtx.variables));
}

TEST(ExpressionIndexOfArray,
     OptimizedExpressionIndexOfArrayWithConstantArgumentsShouldEvaluateProperlyWithRange) {
    auto expCtx = ExpressionContextForTest{};

    auto expIndexOfArray = Expression::parseExpression(
        &expCtx,
        // Search for 4 between 3 and 5.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 3, 4, 5] , '$x', 3, 5] }"),
        expCtx.variablesParseState);
    auto optimizedIndexOfArray = expIndexOfArray->optimize();
    ASSERT_VALUE_EQ(Value(4),
                    optimizedIndexOfArray->evaluate(Document{{"x", 4}}, &expCtx.variables));

    // Should evaluate to -1 if not found in range.
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexOfArray->evaluate(Document{{"x", 0}}, &expCtx.variables));
}

TEST(ExpressionIndexOfArray,
     OptimizedExpressionIndexOfArrayWithConstantArrayShouldEvaluateProperlyWithDuplicateValues) {
    auto expCtx = ExpressionContextForTest{};

    auto expIndexOfArrayWithDuplicateValues =
        Expression::parseExpression(&expCtx,
                                    // Search for 4 between 3 and 5.
                                    fromjson("{ $indexOfArray : [ [0, 1, 2, 2, 3, 4, 5] , '$x'] }"),
                                    expCtx.variablesParseState);
    auto optimizedIndexOfArrayWithDuplicateValues = expIndexOfArrayWithDuplicateValues->optimize();
    ASSERT_VALUE_EQ(
        Value(2),
        optimizedIndexOfArrayWithDuplicateValues->evaluate(Document{{"x", 2}}, &expCtx.variables));
    // Duplicate Values in a range.
    auto expIndexInRangeWithDuplicateValues = Expression::parseExpression(
        &expCtx,
        // Search for 2 between 4 and 6.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 2, 2, 2, 4, 5] , '$x', 4, 6] }"),
        expCtx.variablesParseState);
    auto optimizedIndexInRangeWithDuplicateValues = expIndexInRangeWithDuplicateValues->optimize();
    // Should evaluate to 4.
    ASSERT_VALUE_EQ(
        Value(4),
        optimizedIndexInRangeWithDuplicateValues->evaluate(Document{{"x", 2}}, &expCtx.variables));
}

namespace Parse {

namespace Object {

/**
 * Parses the object given by 'specification', with the options given by 'parseContextOptions'.
 */
boost::intrusive_ptr<Expression> parseObject(BSONObj specification) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    return Expression::parseObject(&expCtx, specification, vps);
};

TEST(ParseObject, ShouldAcceptEmptyObject) {
    auto resultExpression = parseObject(BSONObj());

    // Should return an empty object.
    auto resultObject = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(resultObject);
    ASSERT_VALUE_EQ(resultObject->getValue(), Value(Document{}));
}

TEST(ParseObject, ShouldRecognizeKnownExpression) {
    auto resultExpression = parseObject(BSON("$and" << BSONArray()));

    // Should return an ExpressionAnd.
    auto resultAnd = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(resultAnd);
}

}  // namespace Object

namespace Expression {

using mongo::Expression;

/**
 * Parses an expression from the given BSON specification.
 */
boost::intrusive_ptr<Expression> parseExpression(BSONObj specification) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    return Expression::parseExpression(&expCtx, specification, vps);
}

TEST(ParseExpression, ShouldRecognizeConstExpression) {
    auto resultExpression = parseExpression(BSON("$const" << 5));
    auto constExpression = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(constExpression);
    ASSERT_VALUE_EQ(constExpression->serialize(), Value(Document{{"$const", 5}}));
}

TEST(ParseExpression, ShouldRejectUnknownExpression) {
    ASSERT_THROWS(parseExpression(BSON("$invalid" << 1)), AssertionException);
}

TEST(ParseExpression, ShouldRejectExpressionArgumentsWhichAreNotInArray) {
    ASSERT_THROWS(parseExpression(BSON("$strcasecmp" << "foo")), AssertionException);
}

TEST(ParseExpression, ShouldRejectExpressionWithWrongNumberOfArguments) {
    ASSERT_THROWS(parseExpression(BSON("$strcasecmp" << BSON_ARRAY("foo"))), AssertionException);
}

TEST(ParseExpression, ShouldRejectObjectWithTwoTopLevelExpressions) {
    ASSERT_THROWS(parseExpression(BSON("$and" << BSONArray() << "$or" << BSONArray())),
                  AssertionException);
}

TEST(ParseExpression, ShouldRejectExpressionIfItsNotTheOnlyField) {
    ASSERT_THROWS(parseExpression(BSON("$and" << BSONArray() << "a" << BSON("$or" << BSONArray()))),
                  AssertionException);
}

TEST(ParseExpression, ShouldParseExpressionWithMultipleArguments) {
    auto resultExpression = parseExpression(BSON("$strcasecmp" << BSON_ARRAY("foo" << "FOO")));
    auto strCaseCmpExpression = dynamic_cast<ExpressionStrcasecmp*>(resultExpression.get());
    ASSERT_TRUE(strCaseCmpExpression);
    vector<Value> arguments = {Value(Document{{"$const", "foo"_sd}}),
                               Value(Document{{"$const", "FOO"_sd}})};
    ASSERT_VALUE_EQ(strCaseCmpExpression->serialize(), Value(Document{{"$strcasecmp", arguments}}));
}

TEST(ParseExpression, ShouldParseExpressionWithNoArguments) {
    auto resultExpression = parseExpression(BSON("$and" << BSONArray()));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    ASSERT_VALUE_EQ(andExpression->serialize(), Value(Document{{"$and", vector<Value>{}}}));
}

TEST(ParseExpression, ShouldParseExpressionWithOneArgument) {
    auto resultExpression = parseExpression(BSON("$and" << BSON_ARRAY(1)));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(), Value(Document{{"$and", arguments}}));
}

TEST(ParseExpression, ShouldAcceptArgumentWithoutArrayForVariadicExpressions) {
    auto resultExpression = parseExpression(BSON("$and" << 1));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(), Value(Document{{"$and", arguments}}));
}

TEST(ParseExpression, ShouldAcceptArgumentWithoutArrayAsSingleArgument) {
    auto resultExpression = parseExpression(BSON("$not" << 1));
    auto notExpression = dynamic_cast<ExpressionNot*>(resultExpression.get());
    ASSERT_TRUE(notExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(notExpression->serialize(), Value(Document{{"$not", arguments}}));
}

TEST(ParseExpression, ShouldAcceptObjectAsSingleArgument) {
    auto resultExpression = parseExpression(BSON("$and" << BSON("$const" << 1)));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(), Value(Document{{"$and", arguments}}));
}

TEST(ParseExpression, ShouldAcceptObjectInsideArrayAsSingleArgument) {
    auto resultExpression = parseExpression(BSON("$and" << BSON_ARRAY(BSON("$const" << 1))));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(), Value(Document{{"$and", arguments}}));
}

}  // namespace Expression

namespace Operand {

using mongo::Expression;

/**
 * Parses an operand from the given BSON specification. The field name is ignored, since it is
 * assumed to have come from an array, or to have been the only argument to an expression, in which
 * case the field name would be the name of the expression.
 */
intrusive_ptr<Expression> parseOperand(BSONObj specification) {
    auto expCtx = ExpressionContextForTest{};
    BSONElement specElement = specification.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    return Expression::parseOperand(&expCtx, specElement, vps);
}

TEST(ParseOperand, ShouldRecognizeFieldPath) {
    auto resultExpression = parseOperand(BSON("" << "$field"));
    auto fieldPathExpression = dynamic_cast<ExpressionFieldPath*>(resultExpression.get());
    ASSERT_TRUE(fieldPathExpression);
    ASSERT_VALUE_EQ(fieldPathExpression->serialize(), Value("$field"_sd));
}

TEST(ParseOperand, ShouldRecognizeStringLiteral) {
    auto resultExpression = parseOperand(BSON("" << "foo"));
    auto constantExpression = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(constantExpression);
    ASSERT_VALUE_EQ(constantExpression->serialize(), Value(Document{{"$const", "foo"_sd}}));
}

TEST(ParseOperand, ShouldRecognizeNestedArray) {
    auto resultExpression = parseOperand(BSON("" << BSON_ARRAY("foo" << "$field")));
    auto arrayExpression = dynamic_cast<ExpressionArray*>(resultExpression.get());
    ASSERT_TRUE(arrayExpression);
    vector<Value> expectedSerializedArray = {Value(Document{{"$const", "foo"_sd}}),
                                             Value("$field"_sd)};
    ASSERT_VALUE_EQ(arrayExpression->serialize(), Value(expectedSerializedArray));
}

TEST(ParseOperand, ShouldRecognizeNumberLiteral) {
    auto resultExpression = parseOperand(BSON("" << 5));
    auto constantExpression = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(constantExpression);
    ASSERT_VALUE_EQ(constantExpression->serialize(), Value(Document{{"$const", 5}}));
}

TEST(ParseOperand, ShouldRecognizeNestedExpression) {
    auto resultExpression = parseOperand(BSON("" << BSON("$and" << BSONArray())));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    ASSERT_VALUE_EQ(andExpression->serialize(), Value(Document{{"$and", vector<Value>{}}}));
}

}  // namespace Operand

}  // namespace Parse


namespace BuiltinRemoveVariable {

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = ExpressionFieldPath::parse(&expCtx, "$$REMOVE", vps);
    ASSERT_BSONOBJ_EQ(BSON("foo" << "$$REMOVE"), BSON("foo" << expression->serialize()));
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectlyWithTrailingPath) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = ExpressionFieldPath::parse(&expCtx, "$$REMOVE.a.b", vps);
    ASSERT_BSONOBJ_EQ(BSON("foo" << "$$REMOVE.a.b"), BSON("foo" << expression->serialize()));
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectlyAfterOptimization) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = ExpressionFieldPath::parse(&expCtx, "$$REMOVE.a.b", vps);
    auto optimizedExpression = expression->optimize();
    ASSERT(dynamic_cast<ExpressionConstant*>(optimizedExpression.get()));
    ASSERT_BSONOBJ_EQ(BSON("foo" << "$$REMOVE"), BSON("foo" << optimizedExpression->serialize()));
}

}  // namespace BuiltinRemoveVariable

namespace GetComputedPathsTest {

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWhenUsingRemoveBuiltin) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionFieldPath::parse(&expCtx, "$$REMOVE", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("a", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("a"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWhenOnlyRoot) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionFieldPath::parse(&expCtx, "$$ROOT", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("a", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("a"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWithNonMatchingUserVariable) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.variablesParseState.defineVariable("userVar");
    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar.b", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("a", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("a"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWhenDotted) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionFieldPath::parse(&expCtx, "$a.b", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("c", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("c"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRename) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionFieldPath::parse(&expCtx, "$a", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", Variables::kRootId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRenameWithExplicitRoot) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionFieldPath::parse(&expCtx, "$$ROOT.a", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", Variables::kRootId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRenameWithExplicitCurrent) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionFieldPath::parse(&expCtx, "$$CURRENT.a", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", Variables::kRootId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRenameWithMatchingUserVariable) {
    auto expCtx = ExpressionContextForTest{};
    auto varId = expCtx.variablesParseState.defineVariable("userVar");
    auto expr = ExpressionFieldPath::parse(&expCtx, "$$userVar.a", expCtx.variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", varId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionObjectCorrectlyReportsComputedPaths) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{a: '$b', c: {$add: [1, 3]}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionObject*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("d");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("d.c"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["d.a"], "b");
}

TEST(GetComputedPathsTest, ExpressionObjectCorrectlyReportsComputedPathsNested) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson(
        "{a: {b: '$c'},"
        "d: {$map: {input: '$e', as: 'iter', in: {f: '$$iter.g'}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionObject*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("h");
    // Note that the $map does not contribute to any renames because it can change the shape of a
    // document when the document contains arrays of arrays. Thus the only rename in the output here
    // is the one specified by 'a: {b: '$c'}', and the path computed by the $map is added to the
    // 'paths' list.
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("h.d"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["h.a.b"], "c");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPaths) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject =
        fromjson("{$map: {input: '$a', as: 'iter', in: {b: '$$iter.c', d: {$add: [1, 2]}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    // Note that the $map does not result in any renames because it can change the shape of a
    // document when the document contains arrays of arrays.
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPathsWithDefaultVarName) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a', in: {b: '$$this.c', d: {$add: [1, 2]}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    // Note that the $map does not result in any renames because it can change the shape of a
    // document when the document contains arrays of arrays.
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPathsWithNestedExprObject) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a', in: {b: {c: '$$this.d'}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    // Note that the $map does not result in any renames because it can change the shape of a
    // document when the document contains arrays of array.
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapNotConsideredRenameWithWrongRootVariable) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a', as: 'iter', in: {b: '$c'}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("d");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("d"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapNotConsideredRenameWithWrongVariableNoExpressionObject) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a', as: 'iter', in: '$b'}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("d");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("d"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapNotConsideredRenameWithDottedInputPath) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a.b', as: 'iter', in: {c: '$$iter.d'}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e"), 1u);
    ASSERT(computedPaths.renames.empty());
}

}  // namespace GetComputedPathsTest

namespace expression_meta_test {

TEST(ExpressionMetaTest, ExpressionMetaSearchScoreAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchScore\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchScoreDetailsAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchScoreDetails\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaScoreAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"score\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaScoreDetailsAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"scoreDetails\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetasearchHighlightsAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchHighlights\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaIndexKeyAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"indexKey\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaTextScoreAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.getOperationContext()).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"textScore\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaStreamNotSupported) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"stream\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 9692105);

    // Field path only supported for $meta: "stream.path".
    expr = fromjson("{$meta: \"textScore.foo\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 17308);

    // Empty field path.
    expr = fromjson("{$meta: \"textScore.\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 17308);
    expr = fromjson("{$meta: \"textScore. \"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 17308);
    expr = fromjson("{$meta: \".\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 17308);
    expr = fromjson("{$meta: \".textScore\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 17308);
    expr = fromjson("{$meta: \"\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&expCtx, expr.firstElement(), vps), DBException, 17308);

    // Even with feature flag on, field path only supported for $meta: "stream.path".
    RAIIServerParameterControllerForTest streamsFeatureFlag("featureFlagStreams", true);
    auto ctxWithFlag = ExpressionContextForTest{};
    expr = fromjson("{$meta: \"textScore.foo\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 9692106);

    expr = fromjson("{$meta: \"\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 17308);
    expr = fromjson("{$meta: \".\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 9692107);
    // Empty field path.
    expr = fromjson("{$meta: \"stream.\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 9692107);
    // Bad field path.
    expr = fromjson("{$meta: \"textScore.foo.\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 9692111);
    expr = fromjson("{$meta: \"textScore.$\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 9692111);
    // Field path only supported for $meta: "stream.path".
    expr = fromjson("{$meta: \"textScore.foo\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 9692106);
    // Bad field path.
    expr = fromjson("{$meta: \".textScore\"}");
    ASSERT_THROWS_CODE(
        ExpressionMeta::parse(&ctxWithFlag, expr.firstElement(), vps), DBException, 17308);
}

}  // namespace expression_meta_test

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() override {
        add<Constant::Optimize>();
        add<Constant::Dependencies>();
        add<Constant::AddToBsonObj>();
        add<Constant::AddToBsonArray>();
    }
};

unittest::OldStyleSuiteInitializer<All> myAll;

namespace ExpressionToHashedIndexKeyTest {

TEST(ExpressionToHashedIndexKeyTest, DoesAddInputDependencies) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << "$someValue");
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);

    DepsTracker deps;
    expression::addDependencies(expression.get(), &deps);
    ASSERT_EQ(deps.fields.count("someValue"), 1u);
    ASSERT_EQ(deps.fields.size(), 1u);
}
}  // namespace ExpressionToHashedIndexKeyTest

TEST(ExpressionGetFieldTest, GetFieldTestNullByte) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    StringData str("fo\0o", 4);
    BSONObjBuilder b;
    b.append("$meta"_sd, str);
    BSONObj expr{b.obj()};
    auto expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);
    BSONObj expr1 = fromjson("{$meta: \"foo\"}");
    auto expression1 = ExpressionGetField::parse(&expCtx, expr1.firstElement(), vps);
    auto value1 = expression->serialize();
    auto value2 = expression1->serialize();
    auto str1 = value1.toString();
    auto str2 = value2.toString();
    ASSERT(str1.size() == str2.size() + 1);
    // find the first null byte and delete it in order to compare
    str1.erase(std::find(str1.begin(), str1.end(), '\0'));
    ASSERT(str1 == str2);
    // the 2 values should be equivalent. Despite the null byte inserted in expr.
    ASSERT((value1 == value2).type == Value::DeferredComparison::Type::kEQ);
}

TEST(ExpressionGetFieldTest, GetFieldSerializesStringArgumentCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"foo\"}");
    auto expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$const": "foo"
                    },
                    "input": "$$CURRENT"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));
}

TEST(ExpressionGetFieldTest, GetFieldSerializesCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: {\"field\": \"foo\", \"input\": {a: 1}}}");
    auto expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$const": "foo"
                    },
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    expr = fromjson("{$meta: {\"field\": {$const: \"$foo\"}, \"input\": {a: 1}}}");
    expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$const": "$foo"
                    },
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$const": "$foo"
                    },
                    input: {
                        $const: {"?": "?"}
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$const": "$foo"
                    },
                    "input": "?object"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kDebugQueryShapeSerializeOptions)));
}

TEST(ExpressionGetFieldTest, GetFieldWithDynamicFieldExpressionSerializesCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: {\"field\": {$toString: \"$foo\"}, \"input\": {a: 1}}}");
    auto expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        $convert: {
                            input: "$foo",
                            to: {
                                $const: "string"
                            },
                            format: {
                                $const: "auto"
                            }
                        }
                    },
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$convert": {
                            "input": "$foo",
                            "to": {
                                "$const": "string"
                            },
                            "format": {"$const": "?"}
                        }
                    },
                    "input": {
                        "$const": {
                            "?": "?"
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                $getField: {
                    field: {
                        $convert: {
                            input: "$foo",
                            to: "string",
                            format: "?string"
                        }
                    },
                    input: "?object"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kDebugQueryShapeSerializeOptions)));

    expr = fromjson("{$meta: {\"field\": {$toBool: \"$foo\"}, \"input\": {a: 1}}}");
    expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        $convert: {
                            input: "$foo",
                            to: {
                                $const: "bool"
                            }
                        }
                    },
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                $getField: {
                    field: {
                        $convert: {
                            input: "$foo",
                            to: {
                                $const: "bool"
                            }
                        }
                    },
                    input: {
                        $const: {"?": "?"}
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                $getField: {
                    field: {
                        $convert: {
                            input: "$foo",
                            to: "bool"
                        }
                    },
                    input: "?object"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kDebugQueryShapeSerializeOptions)));

    expr = fromjson(
        "{$meta: {\"field\": {$convert: {\"input\": \"$foo\", \"to\": \"bool\"}}, \"input\": {a: "
        "1}}}");
    expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        $convert: {
                            input: "$foo",
                            to: {
                                $const: "bool"
                            }
                        }
                    },
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                $getField: {
                    field: {
                        $convert: {
                            input: "$foo",
                            to: {
                                $const: "bool"
                            }
                        }
                    },
                    input: {
                        $const: {"?": "?"}
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                $getField: {
                    field: {
                        $convert: {
                            input: "$foo",
                            to: "bool"
                        }
                    },
                    input: "?object"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kDebugQueryShapeSerializeOptions)));

    expr = fromjson(
        "{$meta: {\"field\": {$convert: {\"input\": \"$foo\", \"to\": {\"$add\": [7, 2]}}}, "
        "\"input\": {a: "
        "1}}}");
    expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$convert": {
                            "input": "$foo",
                            "to": {
                                "$add": [
                                    {
                                        "$const": 7
                                    },
                                    {
                                        "$const": 2
                                    }
                                ]
                            }
                        }
                    },
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$convert": {
                            "input": "$foo",
                            "to": {
                                "$add": [1, 1]
                            }
                        }
                    },
                    input: {
                        $const: {"?": "?"}
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": {
                        "$convert": {
                            "input": "$foo",
                            "to": {
                                "$add": "?array<?number>"
                            }
                        }
                    },
                    "input": "?object"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kDebugQueryShapeSerializeOptions)));

    expr = fromjson("{$meta: {\"field\": \"$foo\", \"input\": {a: 1}}}");
    expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": "$foo",
                    "input": {
                        "a": {
                            "$const": 1
                        }
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": "$foo",
                    input: {
                        $const: {"?": "?"}
                    }
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "ignoredField": {
                "$getField": {
                    "field": "$foo",
                    "input": "?object"
                }
            }
        })",
        BSON("ignoredField" << expression->serialize(
                 SerializationOptions::kDebugQueryShapeSerializeOptions)));
}

TEST(ExpressionGetFieldTest, GetFieldSerializesAndRedactsCorrectly) {
    SerializationOptions options = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj expressionBSON = BSON("$getField" << BSON("field" << "a"
                                                              << "input"
                                                              << "$b"));

    auto expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":{"$getField":{"field":"HASH<a>","input":"$HASH<b>"}}})",
        BSON("field" << expression->serialize(options)));

    // Test the shorthand syntax.
    expressionBSON = BSON("$getField" << "a");

    expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":{"$getField":{"field":"HASH<a>","input":"$$CURRENT"}}})",
        BSON("field" << expression->serialize(options)));

    // Test a field with '.' characters.
    expressionBSON = BSON("$getField" << "a.b.c");

    expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$getField": {
                    "field": "HASH<a>.HASH<b>.HASH<c>",
                    "input": "$$CURRENT"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Test a field with a '$' character.
    expressionBSON = BSON("$getField" << "a.$b.c");

    expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$getField": {
                    "field": "HASH<a>.HASH<$b>.HASH<c>",
                    "input": "$$CURRENT"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Test a field with a trailing '.' character (invalid FieldPath).
    expressionBSON = BSON("$getField" << "a.b.c.");

    expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$getField": {
                    "field": "HASH<invalidFieldPathPlaceholder>",
                    "input": "$$CURRENT"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));
}

TEST(ExpressionSetFieldTest, SetFieldRedactsCorrectly) {
    SerializationOptions options = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // Test that a set field redacts properly.
    BSONObj expressionBSON = BSON("$setField" << BSON("field" << "a"
                                                              << "input"
                                                              << "$b"
                                                              << "value"
                                                              << "$c"));
    auto expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": "$HASH<b>",
                    "value": "$HASH<c>"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Object as input.
    expressionBSON =
        BSON("$setField" << BSON("field" << "a"
                                         << "input" << BSON("a" << true) << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": "?object",
                    "value": "?number"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Nested object as input.
    expressionBSON = BSON("$setField" << BSON("field" << "a"
                                                      << "input" << BSON("a" << BSON("b" << 5))
                                                      << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": "?object",
                    "value": "?number"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Object with field path in input.
    expressionBSON =
        BSON("$setField" << BSON("field" << "a"
                                         << "input" << BSON("a" << "$field") << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": {
                        "HASH<a>": "$HASH<field>"
                    },
                    "value": "?number"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Object with field path in value.
    expressionBSON = BSON("$setField" << BSON("field" << "a"
                                                      << "input" << BSON("a" << "b") << "value"
                                                      << BSON("c" << "$d")));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": "?object",
                    "value": {
                        "HASH<c>": "$HASH<d>"
                    }
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Array as input.
    expressionBSON =
        BSON("$setField" << BSON("field" << "a"
                                         << "input" << BSON("a" << BSON_ARRAY(3 << 4 << 5))
                                         << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": "?object",
                    "value": "?number"
                }
            }
        })",
        BSON("field" << expression->serialize(options)));
}

TEST(ExpressionSetFieldTest, SetFieldSerializesCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: {\"field\": \"foo\", \"input\": {a: 1}, \"value\": 24}}");
    auto expression = ExpressionSetField::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_BSONOBJ_EQ(BSON("ignoredField" << BSON(
                               "$setField" << BSON("field" << BSON("$const" << "foo") << "input"
                                                           << BSON("a" << BSON("$const" << 1))
                                                           << "value" << BSON("$const" << 24)))),
                      BSON("ignoredField" << expression->serialize()));
}

TEST(ExpressionSetFieldTest, SetFieldRejectsNullCharInFieldArgument) {
    auto expCtx = ExpressionContextForTest{};
    auto fieldExpr = make_intrusive<ExpressionConstant>(&expCtx, Value("ab\0c"_sd));
    auto inputExpr = make_intrusive<ExpressionConstant>(&expCtx, Value(BSON("a" << 1)));
    auto valueExpr = make_intrusive<ExpressionConstant>(&expCtx, Value(true));
    ASSERT_THROWS_CODE(
        make_intrusive<ExpressionSetField>(
            &expCtx, std::move(fieldExpr), std::move(inputExpr), std::move(valueExpr)),
        AssertionException,
        9534700);
}

TEST(ExpressionIfNullTest, OptimizedExpressionIfNullShouldRemoveNullConstant) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [null, \"$a\", \"$b\"]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedNullRemoved = exprIfNull->optimize();
    auto expectedResult = fromjson("{$ifNull: [\"$a\", \"$b\"]}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedNullRemoved));
}

TEST(ExpressionIfNullTest,
     OptimizedExpressionIfNullShouldRemoveNullConstantAndReturnSingleExpression) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [null, \"$a\"]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedNullRemoved = exprIfNull->optimize();
    ASSERT_VALUE_EQ(optimizedNullRemoved->serialize(), Value("$a"_sd));
}

TEST(ExpressionIfNullTest, OptimizedExpressionIfNullShouldRemoveAllNullConstantsButLast) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [null, \"$a\", null, null]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedNullRemoved = exprIfNull->optimize();
    auto expectedResult = fromjson("{$ifNull: [\"$a\", {$const: null}]}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedNullRemoved));
}

TEST(ExpressionIfNullTest,
     OptimizedExpressionIfNullShouldRemoveAllNullConstantsUnlessItIsOnlyChild) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [null, null]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedNullRemoved = exprIfNull->optimize();
    auto expectedResult = fromjson("{$const: null}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedNullRemoved));
}

TEST(ExpressionIfNullTest, ExpressionIfNullWithAllConstantsShouldOptimizeToExpressionConstant) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [1, 2]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprConstant = exprIfNull->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedExprConstant.get());
    ASSERT_TRUE(exprConstant);
    auto expectedResult = fromjson("{$const: 1}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprConstant));
}

TEST(ExpressionIfNullTest,
     ExpressionIfNullWithNonNullConstantFirstShouldOptimizeByReturningConstantExpression) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [1, \"$a\"]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprConstant = exprIfNull->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedExprConstant.get());
    ASSERT_TRUE(exprConstant);
    auto expectedResult = fromjson("{$const: 1}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprConstant));
}

TEST(ExpressionIfNullTest,
     ExpressionIfNullWithNonNullConstantShouldOptimizeByRemovingFollowingOperands) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [\"$a\", 5, \"$b\"]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedNullRemoved = exprIfNull->optimize();
    auto expectedResult = fromjson("{$ifNull: [\"$a\", {$const: 5}]}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedNullRemoved));
}

TEST(ExpressionIfNullTest,
     ExpressionIfNullWithNullConstantAndNonNullConstantShouldOptimizeToFirstNonNullConstant) {
    auto expCtx = ExpressionContextForTest{};
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$ifNull: [null, 1, \"$a\"]}");
    auto exprIfNull = ExpressionIfNull::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprConstant = exprIfNull->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedExprConstant.get());
    ASSERT_TRUE(exprConstant);
    auto expectedResult = fromjson("{$const: 1}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprConstant));
}

TEST(ExpressionCondTest, ExpressionIfConstantTrueShouldOptimizeToThenClause) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$cond: [true, {$add: [1, 2]}, 2]}");
    auto exprCond = ExpressionCond::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprCond = exprCond->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedExprCond.get());
    ASSERT_TRUE(exprConstant);
    auto expectedResult = fromjson("{$const: 3}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprCond));
}

TEST(ExpressionCondTest, ExpressionIfConstantFalseShouldOptimizeToElseClause) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$cond: [{$gt: [1, 2]}, {$add: [1, 2]}, {$subtract: [3, 1]}]}");
    auto exprCond = ExpressionCond::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprCond = exprCond->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedExprCond.get());
    ASSERT_TRUE(exprConstant);
    auto expectedResult = fromjson("{$const: 2}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprCond));
}

TEST(ExpressionCondTest, ExpressionIfNotConstantShouldNotOptimize) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$cond: [\"$a\", 1, 2]}");
    auto exprCond = ExpressionCond::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprCond = exprCond->optimize();
    auto expectedResult = fromjson("{$cond: [\"$a\", {$const: 1}, {$const: 2}]}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprCond));
}

TEST(ExpressionCondTest, ExpressionIfNotConstantShouldOptimizeBranches) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$cond: [\"$a\", {$multiply: [5, 7]}, {$add: [7, 2]}]}");
    auto exprCond = ExpressionCond::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprCond = exprCond->optimize();
    auto expectedResult = fromjson("{$cond: [\"$a\", {$const: 35}, {$const: 9}]}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprCond));
}

TEST(ExpressionCondTest, ConstantCondShouldOptimizeWithNonConstantBranches) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto expr = fromjson("{$cond: [{$eq: [1, 1]}, {$add: [\"$a\", 2]}, {$subtract: [3, \"$b\"]}]}");
    auto exprCond = ExpressionCond::parse(&expCtx, expr.firstElement(), vps);
    auto optimizedExprCond = exprCond->optimize();
    auto expectedResult = fromjson("{$add: [\"$a\", {$const: 2}]}");
    ASSERT_BSONOBJ_BINARY_EQ(expectedResult, expressionToBson(optimizedExprCond));
}

TEST(ExpressionFLETest, BadInputs) {

    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    {
        auto expr = fromjson("{$_internalFleEq: 12}");
        ASSERT_THROWS_CODE(ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps),
                           DBException,
                           10065);
    }
}

TEST(ExpressionFLETest, ParseAndSerializeBetween) {

    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    auto expr = fromjson(R"({$_internalFleBetween: {
    field: {
        "$binary": {
            "base64":
            "BxI0VngSNJh2EjQSNFZ4kBIQ0JE8aMUFkPk5sSTVqfdNNfjqUfQQ1Uoj0BBcthrWoe9wyU3cN6zmWaQBPJ97t0ZPbecnMsU736yXre6cBO4Zdt/wThtY+v5+7vFgNnWpgRP0e+vam6QPmLvbBrO0LdsvAPTGW4yqwnzCIXCoEg7QPGfbfAXKPDTNenBfRlawiblmTOhO/6ljKotWsMp22q/rpHrn9IEIeJmecwuuPIJ7EA+XYQ3hOKVccYf2ogoK73+8xD/Vul83Qvr84Q8afc4QUMVs8A==",
                "subType": "6"
        }
    },
    server: [{
        "$binary": {
            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
            "subType": "6"
        }
    }]
    } })");

    auto exprFle = ExpressionInternalFLEBetween::parse(&expCtx, expr.firstElement(), vps);
    auto value = exprFle->serialize();

    auto roundTripExpr = fromjson(R"({$_internalFleBetween: {
    field: {
        "$const" : { "$binary": {
            "base64":
            "BxI0VngSNJh2EjQSNFZ4kBIQ0JE8aMUFkPk5sSTVqfdNNfjqUfQQ1Uoj0BBcthrWoe9wyU3cN6zmWaQBPJ97t0ZPbecnMsU736yXre6cBO4Zdt/wThtY+v5+7vFgNnWpgRP0e+vam6QPmLvbBrO0LdsvAPTGW4yqwnzCIXCoEg7QPGfbfAXKPDTNenBfRlawiblmTOhO/6ljKotWsMp22q/rpHrn9IEIeJmecwuuPIJ7EA+XYQ3hOKVccYf2ogoK73+8xD/Vul83Qvr84Q8afc4QUMVs8A==",
                "subType": "6"
        }}
    },
    server: [{
        "$binary": {
            "base64": "COuac/eRLYakKX6B0vZ1r3QodOQFfjqJD+xlGiPu4/Ps",
            "subType": "6"
        }
    }]
        } })");
    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionParseParenthesisExpressionObjTest, MultipleExprSimplification) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson(
        "{input: {$expr: {$expr: {$expr: "
        "{$eq: [123,123]}}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT_EQ(expr->serialize().toString(), "{input: {$eq: [{$const: 123}, {$const: 123}]}}");
}

TEST(ExpressionParseParenthesisExpressionObjTest, SetSingleExprSimplification) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{input: {a: {$expr: {b: 1}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT_EQ(expr->serialize().toString(), "{input: {a: {b: {$const: 1}}}}");
}

TEST(ExpressionParseParenthesisExpressionObjTest, MatchSingleExprSimplification) {

    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{input: {$expr: [false]}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT_EQ(expr->serialize().toString(), "{input: [{$const: false}]}");
}

TEST(ExpressionParseParenthesisExpressionObjTest, SingleExprSimplification) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$expr: [123]}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT_EQ(expr->serialize().toString(), "[{$const: 123}]");
}

TEST(ExpressionParseParenthesisExpressionObjTest, EmptyObject) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$expr: {}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT_EQ(expr->serialize().toString(), "{$const: {}}");
}

TEST(ExpressionSigmoidTest, RoundTripSerialization) {
    auto expCtx = ExpressionContextForTest{};

    auto spec = BSON("$sigmoid" << 100);
    auto sigmoidExp = Expression::parseExpression(&expCtx, spec, expCtx.variablesParseState);

    auto opts = SerializationOptions{LiteralSerializationPolicy::kToRepresentativeParseableValue};
    auto serialized = sigmoidExp->serialize(opts);
    // The query shape for $sigmoid is recorded in its desugared form as there's no
    // ExpressionSigmoid after parsing.
    ASSERT_VALUE_EQ(
        Value(fromjson(R"({$divide: [1, {$add: [1, {$exp: [{$multiply: [1, 1]}]}]}]})")),
        serialized);

    auto roundTrip = Expression::parseExpression(
                         &expCtx, serialized.getDocument().toBson(), expCtx.variablesParseState)
                         ->serialize(opts);
    ASSERT_VALUE_EQ(roundTrip, serialized);
}

TEST(ExpressionSigmoidTest, CorrectRedaction) {
    auto expCtx = ExpressionContextForTest{};

    auto spec = BSON("$sigmoid" << 100);
    auto sigmoidExp = Expression::parseExpression(&expCtx, spec, expCtx.variablesParseState);

    auto opts = SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString};
    auto serialized = sigmoidExp->serialize(opts);
    // The query shape for $sigmoid is recorded in its desugared form as there's no
    // ExpressionSigmoid after parsing.
    ASSERT_VALUE_EQ(
        Value(fromjson(
            R"({$divide: ["?number", {$add: ["?number", {$exp: [{$multiply: "?array<?number>"}]}]}]})")),
        serialized);
}

TEST(ExpressionMapTest, CorrectRedaction) {
    auto expCtx = ExpressionContextForTest{};

    auto spec = fromjson(R"({$map: {input: "$a", as: "x", in : '$$x'}})");
    auto mapExp = Expression::parseExpression(&expCtx, spec, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = mapExp->serialize(opts);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "{$map: {input: \"$HASH<a>\", as: \"HASH<x>\", in: \"$$HASH<x>\"}}",
        serialized);
}

TEST(ExpressionFilterTest, CorrectRedaction) {
    auto expCtx = ExpressionContextForTest{};

    auto spec = fromjson("{$filter: {input: '$a', as: 'x', cond: {$gt: ['$$x', 2]}}}");
    auto filterExp = Expression::parseExpression(&expCtx, spec, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = filterExp->serialize(opts);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "{$filter: {input: \"$HASH<a>\", as: \"HASH<x>\", cond: {$gt: [\"$$HASH<x>\", "
        "\"?number\"]}}}",
        serialized);
}

TEST(ExpressionFilterTest, CorrectRedactionWithLimit) {
    auto expCtx = ExpressionContextForTest{};

    auto spec = fromjson("{$filter: {input: '$a', as: 'x', cond: {$gt: ['$$x', 2]}, limit: 10}}");
    auto filterExp = Expression::parseExpression(&expCtx, spec, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = filterExp->serialize(opts);
    ASSERT_VALUE_EQ_AUTO(  // NOLINT
        "{$filter: {input: \"$HASH<a>\", as: \"HASH<x>\", cond: {$gt: [\"$$HASH<x>\", "
        "\"?number\"]}, limit: \"?number\"}}",
        serialized);
}

TEST(ExpressionFLEStartsWithTest, ParseAssertConstraints) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    {
        auto exprInvalidBson = fromjson("{$encStrStartsWith: 12}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10065);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrStartsWith: {input: {}}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrStartsWith: {input: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    // Error, missing input field.
    {
        auto exprInvalidBson = fromjson("{$encStrStartsWith: {prefix: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            40414);
    }

    // Error, input must be a field path expression.
    {
        auto exprInvalidBson = fromjson("{$encStrStartsWith: {input: \"foo\", prefix:\"test\"}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            16873);
    }

    // Error, prefix must be string or bindata.
    {
        auto exprInvalidBson = fromjson("{$encStrStartsWith: {input: \"$foo\", prefix:2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10111802);
    }

    // Success with string prefix.
    {
        auto exprBson = fromjson("{$encStrStartsWith: {input: \"$foo\", prefix:\"test\"}}");
        auto parsedExpr = ExpressionEncStrStartsWith::parse(&expCtx, exprBson.firstElement(), vps);

        auto* startsWith = dynamic_cast<ExpressionEncStrStartsWith*>(parsedExpr.get());
        ASSERT_NE(startsWith, nullptr);
    }

    // Success with BinData prefix payload.
    {
        auto exprBson = fromjson(R"(
            {$encStrStartsWith: {
                input: "$foo", 
                prefix: {
                    "$binary" : {
                        base64:
                             "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                        subType: "6"
                    }
                }
            }})");
        auto parsedExpr = ExpressionEncStrStartsWith::parse(&expCtx, exprBson.firstElement(), vps);

        auto* startsWith = dynamic_cast<ExpressionEncStrStartsWith*>(parsedExpr.get());
        ASSERT_NE(startsWith, nullptr);
    }

    // Error with incorrect BinData type - must be placeholder or kFLE2FindTextPayload.
    {
        auto exprInvalidBson = fromjson(R"(
            {$encStrStartsWith: {
                input: "$foo",
                prefix: {
                    "$binary" : {
                        base64:
                             "BxI0VngSNJh2EjQSNFZ4kBIQ0JE8aMUFkPk5sSTVqfdNNfjqUfQQ1Uoj0BBcthrWoe9wyU3cN6zmWaQBPJ97t0ZPbecnMsU736yXre6cBO4Zdt/wThtY+v5+7vFgNnWpgRP0e+vam6QPmLvbBrO0LdsvAPTGW4yqwnzCIXCoEg7QPGfbfAXKPDTNenBfRlawiblmTOhO/6ljKotWsMp22q/rpHrn9IEIeJmecwuuPIJ7EA+XYQ3hOKVccYf2ogoK73+8xD/Vul83Qvr84Q8afc4QUMVs8A==",
                        subType: "6"
                    }
                }
            }})");

        ASSERT_THROWS_CODE(
            ExpressionEncStrStartsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10112803);
    }
}

TEST(ExpressionFLEStartsWithTest, ParseStringPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson("{$encStrStartsWith: {input: \"$foo\", prefix:\"test\"}}");

    auto exprFle = ExpressionEncStrStartsWith::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();
    auto roundTripExpr =
        fromjson("{$encStrStartsWith: {input: \"$foo\", prefix: {$const:\"test\"}}}");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEStartsWithTest, ParseBinDataPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson(R"(
        {$encStrStartsWith: {
            input: "$foo", 
            prefix: {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }
            }
        }})");
    auto exprFle = ExpressionEncStrStartsWith::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();

    auto roundTripExpr = fromjson(R"(
        {$encStrStartsWith: {
            input: "$foo", 
            prefix: {
               "$const": {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }
    }}
        }})");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEEndsWithTest, ParseAssertConstraints) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    {
        auto exprInvalidBson = fromjson("{$encStrEndsWith: 12}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10065);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrEndsWith: {input: {}}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrEndsWith: {input: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    // Error, missing input field.
    {
        auto exprInvalidBson = fromjson("{$encStrEndsWith: {suffix: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            40414);
    }

    // Error, input must be a field path expression.
    {
        auto exprInvalidBson = fromjson("{$encStrEndsWith: {input: \"foo\", suffix:\"test\"}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            16873);
    }

    // Error, suffix must be string or bindata.
    {
        auto exprInvalidBson = fromjson("{$encStrEndsWith: {input: \"$foo\", suffix:2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10111802);
    }

    // Success with string suffix.
    {
        auto exprBson = fromjson("{$encStrEndsWith: {input: \"$foo\", suffix:\"test\"}}");
        auto parsedExpr = ExpressionEncStrEndsWith::parse(&expCtx, exprBson.firstElement(), vps);

        auto* endsWith = dynamic_cast<ExpressionEncStrEndsWith*>(parsedExpr.get());
        ASSERT_NE(endsWith, nullptr);
    }

    // Success with BinData suffix payload.
    {
        auto exprBson = fromjson(R"(
            {$encStrEndsWith: {
                input: "$foo", 
                suffix: {
                    "$binary" : {
                        base64:
                             "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                        subType: "6"
                    }
                }
            }})");
        auto parsedExpr = ExpressionEncStrEndsWith::parse(&expCtx, exprBson.firstElement(), vps);

        auto* endsWith = dynamic_cast<ExpressionEncStrEndsWith*>(parsedExpr.get());
        ASSERT_NE(endsWith, nullptr);
    }

    // Error with incorrect BinData type - must be placeholder or kFLE2FindTextPayload.
    {
        auto exprInvalidBson = fromjson(R"(
            {$encStrEndsWith: {
                input: "$foo",
                suffix: {
                    "$binary" : {
                        base64:
                             "BxI0VngSNJh2EjQSNFZ4kBIQ0JE8aMUFkPk5sSTVqfdNNfjqUfQQ1Uoj0BBcthrWoe9wyU3cN6zmWaQBPJ97t0ZPbecnMsU736yXre6cBO4Zdt/wThtY+v5+7vFgNnWpgRP0e+vam6QPmLvbBrO0LdsvAPTGW4yqwnzCIXCoEg7QPGfbfAXKPDTNenBfRlawiblmTOhO/6ljKotWsMp22q/rpHrn9IEIeJmecwuuPIJ7EA+XYQ3hOKVccYf2ogoK73+8xD/Vul83Qvr84Q8afc4QUMVs8A==",
                        subType: "6"
                    }
                }
            }})");

        ASSERT_THROWS_CODE(
            ExpressionEncStrEndsWith::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10112803);
    }
}

TEST(ExpressionFLEEndsWithTest, ParseStringPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson("{$encStrEndsWith: {input: \"$foo\", suffix:\"test\"}}");

    auto exprFle = ExpressionEncStrEndsWith::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();
    auto roundTripExpr =
        fromjson("{$encStrEndsWith: {input: \"$foo\", suffix: {$const:\"test\"}}}");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEEndsWithTest, ParseBinDataPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson(R"(
        {$encStrEndsWith: {
            input: "$foo", 
            suffix: {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }
            }
        }})");
    auto exprFle = ExpressionEncStrEndsWith::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();

    auto roundTripExpr = fromjson(R"(
        {$encStrEndsWith: {
            input: "$foo", 
            suffix: {
               "$const": {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }
    }}
        }})");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEStrContainsTest, ParseAssertConstraints) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    {
        auto exprInvalidBson = fromjson("{$encStrContains: 12}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10065);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrContains: {input: {}}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrContains: {input: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    // Error, missing input field.
    {
        auto exprInvalidBson = fromjson("{$encStrContains: {substring: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            40414);
    }

    // Error, input must be a field path expression.
    {
        auto exprInvalidBson = fromjson("{$encStrContains: {input: \"foo\", substring:\"test\"}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            16873);
    }

    // Error, substring must be string or bindata.
    {
        auto exprInvalidBson = fromjson("{$encStrContains: {input: \"$foo\", substring:2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10111802);
    }

    // Success with string substring.
    {
        auto exprBson = fromjson("{$encStrContains: {input: \"$foo\", substring:\"test\"}}");
        auto parsedExpr = ExpressionEncStrContains::parse(&expCtx, exprBson.firstElement(), vps);

        auto* exprContains = dynamic_cast<ExpressionEncStrContains*>(parsedExpr.get());
        ASSERT_NE(exprContains, nullptr);
    }

    // Success with BinData substring payload.
    {
        auto exprBson = fromjson(R"(
            {$encStrContains: {
                input: "$foo", 
                substring: {
                    "$binary" : {
                        base64:
                             "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                        subType: "6"
                    }
                }
            }})");
        auto parsedExpr = ExpressionEncStrContains::parse(&expCtx, exprBson.firstElement(), vps);

        auto* contains = dynamic_cast<ExpressionEncStrContains*>(parsedExpr.get());
        ASSERT_NE(contains, nullptr);
    }

    // Error with incorrect BinData type - must be placeholder or kFLE2FindTextPayload.
    {
        auto exprInvalidBson = fromjson(R"(
            {$encStrContains: {
                input: "$foo",
                substring: {
                    "$binary" : {
                        base64:
                             "BxI0VngSNJh2EjQSNFZ4kBIQ0JE8aMUFkPk5sSTVqfdNNfjqUfQQ1Uoj0BBcthrWoe9wyU3cN6zmWaQBPJ97t0ZPbecnMsU736yXre6cBO4Zdt/wThtY+v5+7vFgNnWpgRP0e+vam6QPmLvbBrO0LdsvAPTGW4yqwnzCIXCoEg7QPGfbfAXKPDTNenBfRlawiblmTOhO/6ljKotWsMp22q/rpHrn9IEIeJmecwuuPIJ7EA+XYQ3hOKVccYf2ogoK73+8xD/Vul83Qvr84Q8afc4QUMVs8A==",
                        subType: "6"
                    }
                }
            }})");

        ASSERT_THROWS_CODE(
            ExpressionEncStrContains::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10112803);
    }
}

TEST(ExpressionFLEStrContainsTest, ParseStringPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson("{$encStrContains: {input: \"$foo\", substring:\"test\"}}");

    auto exprFle = ExpressionEncStrContains::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();
    auto roundTripExpr =
        fromjson("{$encStrContains: {input: \"$foo\", substring: {$const:\"test\"}}}");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEStrContainsTest, ParseBinDataPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson(R"(
        {$encStrContains: {
            input: "$foo", 
            substring: {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }
            }
        }})");
    auto exprFle = ExpressionEncStrContains::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();

    auto roundTripExpr = fromjson(R"(
        {$encStrContains: {
            input: "$foo", 
            substring: {
               "$const": {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }}}}})");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEStrNormalizedEqTest, ParseAssertConstraints) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    {
        auto exprInvalidBson = fromjson("{$encStrNormalizedEq: 12}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10065);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrNormalizedEq: {input: {}}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    {
        auto exprInvalidBson = fromjson("{$encStrNormalizedEq: {input: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            14);
    }

    // Error, missing input field.
    {
        auto exprInvalidBson = fromjson("{$encStrNormalizedEq: {string: 2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            40414);
    }

    // Error, input must be a field path expression.
    {
        auto exprInvalidBson = fromjson("{$encStrNormalizedEq: {input: \"foo\", string:\"test\"}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            16873);
    }

    // Error, string must be string or bindata.
    {
        auto exprInvalidBson = fromjson("{$encStrNormalizedEq: {input: \"$foo\", string:2}}");
        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10111802);
    }

    // Success with string string.
    {
        auto exprBson = fromjson("{$encStrNormalizedEq: {input: \"$foo\", string:\"test\"}}");
        auto parsedExpr =
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprBson.firstElement(), vps);

        auto* exprNormalizedEq = dynamic_cast<ExpressionEncStrNormalizedEq*>(parsedExpr.get());
        ASSERT_NE(exprNormalizedEq, nullptr);
    }

    // Success with BinData string payload.
    {
        auto exprBson = fromjson(R"(
            {$encStrNormalizedEq: {
                input: "$foo", 
                string: {
                    "$binary" : {
                        base64:
                             "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                        subType: "6"
                    }
                }
            }})");
        auto parsedExpr =
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprBson.firstElement(), vps);

        auto* normalizedEq = dynamic_cast<ExpressionEncStrNormalizedEq*>(parsedExpr.get());
        ASSERT_NE(normalizedEq, nullptr);
    }

    // Error with incorrect BinData type - must be placeholder or kFLE2FindTextPayload.
    {
        auto exprInvalidBson = fromjson(R"(
            {$encStrNormalizedEq: {
                input: "$foo",
                string: {
                    "$binary" : {
                        base64:
                             "BxI0VngSNJh2EjQSNFZ4kBIQ0JE8aMUFkPk5sSTVqfdNNfjqUfQQ1Uoj0BBcthrWoe9wyU3cN6zmWaQBPJ97t0ZPbecnMsU736yXre6cBO4Zdt/wThtY+v5+7vFgNnWpgRP0e+vam6QPmLvbBrO0LdsvAPTGW4yqwnzCIXCoEg7QPGfbfAXKPDTNenBfRlawiblmTOhO/6ljKotWsMp22q/rpHrn9IEIeJmecwuuPIJ7EA+XYQ3hOKVccYf2ogoK73+8xD/Vul83Qvr84Q8afc4QUMVs8A==",
                        subType: "6"
                    }
                }
            }})");

        ASSERT_THROWS_CODE(
            ExpressionEncStrNormalizedEq::parse(&expCtx, exprInvalidBson.firstElement(), vps),
            DBException,
            10112803);
    }
}

TEST(ExpressionFLEStrNormalizedEqTest, ParseStringPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson("{$encStrNormalizedEq: {input: \"$foo\", string:\"test\"}}");

    auto exprFle = ExpressionEncStrNormalizedEq::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();
    auto roundTripExpr =
        fromjson("{$encStrNormalizedEq: {input: \"$foo\", string: {$const:\"test\"}}}");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionFLEStrNormalizedEqTest, ParseBinDataPayloadRoundtrip) {
    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;
    auto exprBson = fromjson(R"(
        {$encStrNormalizedEq: {
            input: "$foo", 
            string: {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }
            }
        }})");
    auto exprFle = ExpressionEncStrNormalizedEq::parse(&expCtx, exprBson.firstElement(), vps);
    auto value = exprFle->serialize();

    auto roundTripExpr = fromjson(R"(
        {$encStrNormalizedEq: {
            input: "$foo", 
            string: {
               "$const": {
                "$binary" : {
                    base64:
                         "A5AAAAEEdAACAAAAEGEARAAAAFWtpAQAAAABOUkiIcv1EcpWTI5w7Tcwls1AFQAAAAAE5SSIcv1EcpWTI5w7Tcwls1AFwAAAAAnYAMAAAAV2kAhjYXNlZgAIaGlhY2YAAnByZWZpeAAVAAAAEG9bAgAAAAGMYgACAAAAAABJjbAAwAAAAA==",
                    subType: "6"
                }}}}})");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
}

TEST(ExpressionSplitTest, CorrectSerializationTest) {
    auto expCtx = ExpressionContextForTest{};

    auto exprBSON = fromjson(R"({$split: ["abcabc", "b"]})");
    auto splitExp = Expression::parseExpression(&expCtx, exprBSON, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = splitExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(fromjson(R"({$split: ["?string", "?string"]})"), serialized);

    opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    serialized = splitExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(fromjson(R"({$split: ["?string", "?string"]})"), serialized);

    opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    serialized = splitExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(fromjson(R"({$split: ["?", "?"]})"), serialized);
}

TEST(ExpressionSplitTest, RegExCorrectSerializationTest) {
    auto expCtx = ExpressionContextForTest{};

    auto exprBSON = fromjson(R"({$split: ["abcabc", /.*/]})");
    auto splitExp = Expression::parseExpression(&expCtx, exprBSON, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = splitExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(fromjson(R"({$split: ["?string", "?regex"]})"), serialized);

    opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    serialized = splitExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(fromjson(R"({$split: ["?string", "?regex"]})"), serialized);

    opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    auto serializedStr = splitExp->serialize(opts);
    ASSERT_VALUE_EQ_AUTO("{$split: [\"?\", //?//]}", serializedStr);
}

TEST(ExpressionReplaceOneTest, CorrectSerializationTest) {
    auto expCtx = ExpressionContextForTest{};

    auto exprBSON = fromjson(R"({$replaceOne: {input: "abcabc", find: "b", replacement: "d"}})");
    auto replaceExp = Expression::parseExpression(&expCtx, exprBSON, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceOne: {input: "?string", find: "?string", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceOne: {input: "?string", find: "?string", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(
            R"({$replaceOne: {input: {$const: "?"}, find: {$const: "?"}, replacement: {$const: "?"}}})"),
        serialized);
}

TEST(ExpressionReplaceOneTest, RegExCorrectSerializationTest) {
    auto expCtx = ExpressionContextForTest{};

    auto exprBSON = fromjson(R"({$replaceOne: {input: "abcabc", find: /.*/, replacement: "d"}})");
    auto replaceExp = Expression::parseExpression(&expCtx, exprBSON, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceOne: {input: "?string", find: "?regex", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceOne: {input: "?string", find: "?regex", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    auto serializedStr = replaceExp->serialize(opts);
    ASSERT_VALUE_EQ_AUTO(
        "{$replaceOne: {input: {$const: \"?\"}, find: {$const: //?//}, replacement: {$const: "
        "\"?\"}}}",
        serializedStr);
}

TEST(ExpressionReplaceAllTest, CorrectSerializationTest) {
    auto expCtx = ExpressionContextForTest{};

    auto exprBSON = fromjson(R"({$replaceAll: {input: "abcabc", find: "b", replacement: "d"}})");
    auto replaceExp = Expression::parseExpression(&expCtx, exprBSON, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceAll: {input: "?string", find: "?string", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceAll: {input: "?string", find: "?string", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(
            R"({$replaceAll: {input: {$const: "?"}, find: {$const: "?"}, replacement: {$const: "?"}}})"),
        serialized);
}

TEST(ExpressionReplaceAllTest, RegExCorrectSerializationTest) {
    auto expCtx = ExpressionContextForTest{};

    auto exprBSON = fromjson(R"({$replaceAll: {input: "abcabc", find: /.*/, replacement: "d"}})");
    auto replaceExp = Expression::parseExpression(&expCtx, exprBSON, expCtx.variablesParseState);

    auto opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceAll: {input: "?string", find: "?regex", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kDebugQueryShapeSerializeOptions;
    serialized = replaceExp->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        fromjson(R"({$replaceAll: {input: "?string", find: "?regex", replacement: "?string"}})"),
        serialized);

    opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    auto serializedStr = replaceExp->serialize(opts);
    ASSERT_VALUE_EQ_AUTO(
        "{$replaceAll: {input: {$const: \"?\"}, find: {$const: //?//}, replacement: {$const: "
        "\"?\"}}}",
        serializedStr);
}

}  // namespace ExpressionTests
}  // namespace mongo

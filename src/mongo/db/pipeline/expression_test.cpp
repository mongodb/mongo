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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/config.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace ExpressionTests {

using boost::intrusive_ptr;
using std::initializer_list;
using std::list;
using std::numeric_limits;
using std::pair;
using std::set;
using std::sort;
using std::string;
using std::vector;

/**
 * Creates an expression given by 'expressionName' and evaluates it using
 * 'operands' as inputs, returning the result.
 */
static Value evaluateExpression(const string& expressionName,
                                const vector<ImplicitValue>& operands) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    const BSONObj obj = BSON(expressionName << ImplicitValue::convertToValue(operands));
    auto expression = Expression::parseExpression(expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx->variables);
    return result;
}

/**
 * Takes the name of an expression as its first argument and a list of pairs of arguments and
 * expected results as its second argument, and asserts that for the given expression the arguments
 * evaluate to the expected results.
 */
static void assertExpectedResults(
    const string& expression,
    initializer_list<pair<vector<ImplicitValue>, ImplicitValue>> operations) {
    for (auto&& op : operations) {
        try {
            Value result = evaluateExpression(expression, op.first);
            ASSERT_VALUE_EQ(op.second, result);
            ASSERT_EQUALS(op.second.getType(), result.getType());
        } catch (...) {
            LOGV2(24188, "failed", "argument"_attr = ImplicitValue::convertToValue(op.first));
            throw;
        }
    }
}

/** Convert BSONObj to a BSONObj with our $const wrappings. */
static BSONObj constify(const BSONObj& obj, bool parentIsArray = false) {
    BSONObjBuilder bob;
    for (BSONObjIterator itr(obj); itr.more(); itr.next()) {
        BSONElement elem = *itr;
        if (elem.type() == Object) {
            bob << elem.fieldName() << constify(elem.Obj(), false);
        } else if (elem.type() == Array && !parentIsArray) {
            // arrays within arrays are treated as constant values by the real
            // parser
            bob << elem.fieldName() << BSONArray(constify(elem.Obj(), true));
        } else if (elem.fieldNameStringData() == "$const" ||
                   (elem.type() == mongo::String && elem.valueStringDataSafe().startsWith("$"))) {
            bob.append(elem);
        } else {
            bob.append(elem.fieldName(), BSON("$const" << elem));
        }
    }
    return bob.obj();
}

/** Check binary equality, ensuring use of the same numeric types. */
static void assertBinaryEqual(const BSONObj& expected, const BSONObj& actual) {
    ASSERT_BSONOBJ_EQ(expected, actual);
    ASSERT(expected.binaryEqual(actual));
}

/** Convert Value to a wrapped BSONObj with an empty string field name. */
static BSONObj toBson(const Value& value) {
    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

/** Convert Expression to BSON. */
static BSONObj expressionToBson(const intrusive_ptr<Expression>& expression) {
    return BSON("" << expression->serialize(false)).firstElement().embeddedObject().getOwned();
}

/** Convert Document to BSON. */
static BSONObj toBson(const Document& document) {
    return document.toBson();
}

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj) {
    return Document(obj);
}

/** Create a Value from a BSONObj. */
Value valueFromBson(BSONObj obj) {
    BSONElement element = obj.firstElement();
    return Value(element);
}

/* ------------------------- ExpressionArrayToObject -------------------------- */

TEST(ExpressionArrayToObjectTest, KVFormatSimple) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON("k"
                                                   << "key1"
                                                   << "v" << 2)
                                              << BSON("k"
                                                      << "key2"
                                                      << "v" << 3)))},
                            {Value(BSON("key1" << 2 << "key2" << 3))}}});
}

TEST(ExpressionArrayToObjectTest, KVFormatWithDuplicates) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON("k"
                                                   << "hi"
                                                   << "v" << 2)
                                              << BSON("k"
                                                      << "hi"
                                                      << "v" << 3)))},
                            {Value(BSON("hi" << 3))}}});
}

TEST(ExpressionArrayToObjectTest, ListFormatSimple) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON_ARRAY("key1" << 2) << BSON_ARRAY("key2" << 3)))},
                            {Value(BSON("key1" << 2 << "key2" << 3))}}});
}

TEST(ExpressionArrayToObjectTest, ListFormWithDuplicates) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON_ARRAY("key1" << 2) << BSON_ARRAY("key1" << 3)))},
                            {Value(BSON("key1" << 3))}}});
}

/* ------------------------ ExpressionRange --------------------------- */

TEST(ExpressionRangeTest, ComputesStandardRange) {
    assertExpectedResults("$range", {{{Value(0), Value(3)}, Value(BSON_ARRAY(0 << 1 << 2))}});
}

TEST(ExpressionRangeTest, ComputesRangeWithStep) {
    assertExpectedResults("$range",
                          {{{Value(0), Value(6), Value(2)}, Value(BSON_ARRAY(0 << 2 << 4))}});
}

TEST(ExpressionRangeTest, ComputesReverseRange) {
    assertExpectedResults("$range",
                          {{{Value(0), Value(-3), Value(-1)}, Value(BSON_ARRAY(0 << -1 << -2))}});
}

TEST(ExpressionRangeTest, ComputesRangeWithPositiveAndNegative) {
    assertExpectedResults("$range",
                          {{{Value(-2), Value(3)}, Value(BSON_ARRAY(-2 << -1 << 0 << 1 << 2))}});
}

TEST(ExpressionRangeTest, ComputesEmptyRange) {
    assertExpectedResults("$range",
                          {{{Value(-2), Value(3), Value(-1)}, Value(std::vector<Value>())}});
}

TEST(ExpressionRangeTest, ComputesRangeWithSameStartAndEnd) {
    assertExpectedResults("$range", {{{Value(20), Value(20)}, Value(std::vector<Value>())}});
}

TEST(ExpressionRangeTest, ComputesRangeWithLargeNegativeStep) {
    assertExpectedResults("$range",
                          {{{Value(3), Value(-5), Value(-3)}, Value(BSON_ARRAY(3 << 0 << -3))}});
}

/* ------------------------ ExpressionReverseArray -------------------- */

TEST(ExpressionReverseArrayTest, ReversesNormalArray) {
    assertExpectedResults("$reverseArray",
                          {{{Value(BSON_ARRAY(1 << 2 << 3))}, Value(BSON_ARRAY(3 << 2 << 1))}});
}

TEST(ExpressionReverseArrayTest, ReversesEmptyArray) {
    assertExpectedResults("$reverseArray",
                          {{{Value(std::vector<Value>())}, Value(std::vector<Value>())}});
}

TEST(ExpressionReverseArrayTest, ReversesOneElementArray) {
    assertExpectedResults("$reverseArray", {{{Value(BSON_ARRAY(1))}, Value(BSON_ARRAY(1))}});
}

TEST(ExpressionReverseArrayTest, ReturnsNullWithNullishInput) {
    assertExpectedResults(
        "$reverseArray",
        {{{Value(BSONNULL)}, Value(BSONNULL)}, {{Value(BSONUndefined)}, Value(BSONNULL)}});
}

/* ------------------------- Old-style tests -------------------------- */

namespace Add {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(expCtx);
        populateOperands(expression);
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(expression->evaluate({}, &expCtx->variables)));
    }

protected:
    virtual void populateOperands(intrusive_ptr<ExpressionNary>& expression) = 0;
    virtual BSONObj expectedResult() = 0;
};

/** $add with a NULL Document pointer, as called by ExpressionNary::optimize().
 */
class NullDocument {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(expCtx);
        expression->addOperand(ExpressionConstant::create(expCtx, Value(2)));
        ASSERT_BSONOBJ_EQ(BSON("" << 2), toBson(expression->evaluate({}, &expCtx->variables)));
    }
};

/** $add without operands. */
class NoOperands : public ExpectedResultBase {
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {}
    virtual BSONObj expectedResult() {
        return BSON("" << 0);
    }
};

/** String type unsupported. */
class String {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(expCtx);
        expression->addOperand(ExpressionConstant::create(expCtx, Value("a"_sd)));
        ASSERT_THROWS(expression->evaluate({}, &expCtx->variables), AssertionException);
    }
};

/** Bool type unsupported. */
class Bool {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(expCtx);
        expression->addOperand(ExpressionConstant::create(expCtx, Value(true)));
        ASSERT_THROWS(expression->evaluate({}, &expCtx->variables), AssertionException);
    }
};

class SingleOperandBase : public ExpectedResultBase {
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        expression->addOperand(ExpressionConstant::create(expCtx, valueFromBson(operand())));
    }
    BSONObj expectedResult() {
        return operand();
    }

protected:
    virtual BSONObj operand() = 0;
};

/** Single int argument. */
class Int : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << 1);
    }
};

/** Single long argument. */
class Long : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << 5555LL);
    }
};

/** Single double argument. */
class Double : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << 99.99);
    }
};

/** Single Date argument. */
class Date : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << Date_t::fromMillisSinceEpoch(12345));
    }
};

/** Single null argument. */
class Null : public SingleOperandBase {
    BSONObj operand() {
        return BSON("" << BSONNULL);
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

/** Single undefined argument. */
class Undefined : public SingleOperandBase {
    BSONObj operand() {
        return fromjson("{'':undefined}");
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

class TwoOperandBase : public ExpectedResultBase {
public:
    TwoOperandBase() : _reverse() {}
    void run() {
        ExpectedResultBase::run();
        // Now add the operands in the reverse direction.
        _reverse = true;
        ExpectedResultBase::run();
    }

protected:
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        expression->addOperand(
            ExpressionConstant::create(expCtx, valueFromBson(_reverse ? operand2() : operand1())));
        expression->addOperand(
            ExpressionConstant::create(expCtx, valueFromBson(_reverse ? operand1() : operand2())));
    }
    virtual BSONObj operand1() = 0;
    virtual BSONObj operand2() = 0;

private:
    bool _reverse;
};

/** Add two ints. */
class IntInt : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 1);
    }
    BSONObj operand2() {
        return BSON("" << 5);
    }
    BSONObj expectedResult() {
        return BSON("" << 6);
    }
};

/** Adding two large ints produces a long, not an overflowed int. */
class IntIntNoOverflow : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << numeric_limits<int>::max());
    }
    BSONObj operand2() {
        return BSON("" << numeric_limits<int>::max());
    }
    BSONObj expectedResult() {
        return BSON("" << ((long long)(numeric_limits<int>::max()) + numeric_limits<int>::max()));
    }
};

/** Adding an int and a long produces a long. */
class IntLong : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 1);
    }
    BSONObj operand2() {
        return BSON("" << 9LL);
    }
    BSONObj expectedResult() {
        return BSON("" << 10LL);
    }
};

/** Adding an int and a long produces a double. */
class IntLongOverflowToDouble : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << numeric_limits<int>::max());
    }
    BSONObj operand2() {
        return BSON("" << numeric_limits<long long>::max());
    }
    BSONObj expectedResult() {
        // When the result cannot be represented in a NumberLong, a NumberDouble is returned.
        const auto im = numeric_limits<int>::max();
        const auto llm = numeric_limits<long long>::max();
        double result = static_cast<double>(im) + static_cast<double>(llm);
        return BSON("" << result);
    }
};

/** Adding an int and a double produces a double. */
class IntDouble : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 9);
    }
    BSONObj operand2() {
        return BSON("" << 1.1);
    }
    BSONObj expectedResult() {
        return BSON("" << 10.1);
    }
};

/** Adding an int and a Date produces a Date. */
class IntDate : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 6);
    }
    BSONObj operand2() {
        return BSON("" << Date_t::fromMillisSinceEpoch(123450));
    }
    BSONObj expectedResult() {
        return BSON("" << Date_t::fromMillisSinceEpoch(123456));
    }
};

/** Adding a long and a double produces a double. */
class LongDouble : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 9LL);
    }
    BSONObj operand2() {
        return BSON("" << 1.1);
    }
    BSONObj expectedResult() {
        return BSON("" << 10.1);
    }
};

/** Adding a long and a double does not overflow. */
class LongDoubleNoOverflow : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << numeric_limits<long long>::max());
    }
    BSONObj operand2() {
        return BSON("" << double(numeric_limits<long long>::max()));
    }
    BSONObj expectedResult() {
        return BSON("" << numeric_limits<long long>::max() +
                        double(numeric_limits<long long>::max()));
    }
};

/** Adding an int and null. */
class IntNull : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 1);
    }
    BSONObj operand2() {
        return BSON("" << BSONNULL);
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

/** Adding a long and undefined. */
class LongUndefined : public TwoOperandBase {
    BSONObj operand1() {
        return BSON("" << 5LL);
    }
    BSONObj operand2() {
        return fromjson("{'':undefined}");
    }
    BSONObj expectedResult() {
        return BSON("" << BSONNULL);
    }
};

}  // namespace Add

namespace CoerceToBool {

/** Nested expression coerced to true. */
class EvaluateTrue {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> nested = ExpressionConstant::create(expCtx, Value(5));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(expCtx, nested);
        ASSERT(expression->evaluate({}, &expCtx->variables).getBool());
    }
};

/** Nested expression coerced to false. */
class EvaluateFalse {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> nested = ExpressionConstant::create(expCtx, Value(0));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(expCtx, nested);
        ASSERT(!expression->evaluate({}, &expCtx->variables).getBool());
    }
};

/** Dependencies forwarded from nested expression. */
class Dependencies {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> nested = ExpressionFieldPath::create(expCtx, "a.b");
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(expCtx, nested);
        DepsTracker dependencies;
        expression->addDependencies(&dependencies);
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedsAnyMetadata());
    }
};

/** Output to BSONObj. */
class AddToBsonObj {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression =
            ExpressionCoerceToBool::create(expCtx, ExpressionFieldPath::create(expCtx, "foo"));

        // serialized as $and because CoerceToBool isn't an ExpressionNary
        assertBinaryEqual(fromjson("{field:{$and:['$foo']}}"), toBsonObj(expression));
    }

private:
    static BSONObj toBsonObj(const intrusive_ptr<Expression>& expression) {
        return BSON("field" << expression->serialize(false));
    }
};

/** Output to BSONArray. */
class AddToBsonArray {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression =
            ExpressionCoerceToBool::create(expCtx, ExpressionFieldPath::create(expCtx, "foo"));

        // serialized as $and because CoerceToBool isn't an ExpressionNary
        assertBinaryEqual(BSON_ARRAY(fromjson("{$and:['$foo']}")), toBsonArray(expression));
    }

private:
    static BSONArray toBsonArray(const intrusive_ptr<Expression>& expression) {
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        return bab.arr();
    }
};

// TODO Test optimize(), difficult because a CoerceToBool cannot be output as
// BSON.

}  // namespace CoerceToBool

namespace Constant {

/** Create an ExpressionConstant from a Value. */
class Create {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value(5));
        assertBinaryEqual(BSON("" << 5), toBson(expression->evaluate({}, &expCtx->variables)));
    }
};

/** Create an ExpressionConstant from a BsonElement. */
class CreateFromBsonElement {
public:
    void run() {
        BSONObj spec = BSON("IGNORED_FIELD_NAME"
                            << "foo");
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONElement specElement = spec.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = ExpressionConstant::parse(expCtx, specElement, vps);
        assertBinaryEqual(BSON(""
                               << "foo"),
                          toBson(expression->evaluate({}, &expCtx->variables)));
    }
};

/** No optimization is performed. */
class Optimize {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value(5));
        // An attempt to optimize returns the Expression itself.
        ASSERT_EQUALS(expression, expression->optimize());
    }
};

/** No dependencies. */
class Dependencies {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value(5));
        DepsTracker dependencies;
        expression->addDependencies(&dependencies);
        ASSERT_EQUALS(0U, dependencies.fields.size());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedsAnyMetadata());
    }
};

/** Output to BSONObj. */
class AddToBsonObj {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value(5));
        // The constant is replaced with a $ expression.
        assertBinaryEqual(BSON("field" << BSON("$const" << 5)), toBsonObj(expression));
    }

private:
    static BSONObj toBsonObj(const intrusive_ptr<Expression>& expression) {
        return BSON("field" << expression->serialize(false));
    }
};

/** Output to BSONArray. */
class AddToBsonArray {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value(5));
        // The constant is copied out as is.
        assertBinaryEqual(constify(BSON_ARRAY(5)), toBsonArray(expression));
    }

private:
    static BSONObj toBsonArray(const intrusive_ptr<Expression>& expression) {
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        return bab.obj();
    }
};

TEST(ExpressionConstantTest, ConstantOfValueMissingRemovesField) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value());
    assertBinaryEqual(
        BSONObj(),
        toBson(expression->evaluate(Document{{"foo", Value("bar"_sd)}}, &expCtx->variables)));
}

TEST(ExpressionConstantTest, ConstantOfValueMissingSerializesToRemoveSystemVar) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value());
    assertBinaryEqual(BSON("field"
                           << "$$REMOVE"),
                      BSON("field" << expression->serialize(false)));
}

}  // namespace Constant

TEST(ExpressionFromAccumulators, Avg) {
    assertExpectedResults("$avg",
                          {// $avg ignores non-numeric inputs.
                           {{Value("string"_sd), Value(BSONNULL), Value(), Value(3)}, Value(3.0)},
                           // $avg always returns a double.
                           {{Value(10LL), Value(20LL)}, Value(15.0)},
                           // $avg returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, Max) {
    assertExpectedResults("$max",
                          {// $max treats non-numeric inputs as valid arguments.
                           {{Value(1), Value(BSONNULL), Value(), Value("a"_sd)}, Value("a"_sd)},
                           {{Value("a"_sd), Value("b"_sd)}, Value("b"_sd)},
                           // $max always preserves the type of the result.
                           {{Value(10LL), Value(0.0), Value(5)}, Value(10LL)},
                           // $max returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, Min) {
    assertExpectedResults("$min",
                          {// $min treats non-numeric inputs as valid arguments.
                           {{Value("string"_sd)}, Value("string"_sd)},
                           {{Value(1), Value(BSONNULL), Value(), Value("a"_sd)}, Value(1)},
                           {{Value("a"_sd), Value("b"_sd)}, Value("a"_sd)},
                           // $min always preserves the type of the result.
                           {{Value(0LL), Value(20.0), Value(10)}, Value(0LL)},
                           // $min returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, Sum) {
    assertExpectedResults(
        "$sum",
        {// $sum ignores non-numeric inputs.
         {{Value("string"_sd), Value(BSONNULL), Value(), Value(3)}, Value(3)},
         // If any argument is a double, $sum returns a double
         {{Value(10LL), Value(10.0)}, Value(20.0)},
         // If no arguments are doubles and an argument is a long, $sum returns a long
         {{Value(10LL), Value(10)}, Value(20LL)},
         // $sum returns 0 when no arguments are provided.
         {{}, Value(0)}});
}

TEST(ExpressionFromAccumulators, StdDevPop) {
    assertExpectedResults("$stdDevPop",
                          {// $stdDevPop ignores non-numeric inputs.
                           {{Value("string"_sd), Value(BSONNULL), Value(), Value(3)}, Value(0.0)},
                           // $stdDevPop always returns a double.
                           {{Value(1LL), Value(3LL)}, Value(1.0)},
                           // $stdDevPop returns null when no arguments are provided.
                           {{}, Value(BSONNULL)}});
}

TEST(ExpressionFromAccumulators, StdDevSamp) {
    assertExpectedResults(
        "$stdDevSamp",
        {// $stdDevSamp ignores non-numeric inputs.
         {{Value("string"_sd), Value(BSONNULL), Value(), Value(3)}, Value(BSONNULL)},
         // $stdDevSamp always returns a double.
         {{Value(1LL), Value(2LL), Value(3LL)}, Value(1.0)},
         // $stdDevSamp returns null when no arguments are provided.
         {{}, Value(BSONNULL)}});
}

TEST(ExpressionPowTest, LargeExponentValuesWithBaseOfZero) {
    assertExpectedResults(
        "$pow",
        {
            {{Value(0), Value(0)}, Value(1)},
            {{Value(0LL), Value(0LL)}, Value(1LL)},

            {{Value(0), Value(10)}, Value(0)},
            {{Value(0), Value(10000)}, Value(0)},

            {{Value(0LL), Value(10)}, Value(0LL)},

            // $pow may sometimes use a loop to compute a^b, so it's important to check
            // that the loop doesn't hang if a large exponent is provided.
            {{Value(0LL), Value(std::numeric_limits<long long>::max())}, Value(0LL)},
        });
}

TEST(ExpressionPowTest, ThrowsWhenBaseZeroAndExpNegative) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    const auto expr = Expression::parseExpression(expCtx, BSON("$pow" << BSON_ARRAY(0 << -5)), vps);
    ASSERT_THROWS([&] { expr->evaluate({}, &expCtx->variables); }(), AssertionException);

    const auto exprWithLong =
        Expression::parseExpression(expCtx, BSON("$pow" << BSON_ARRAY(0LL << -5LL)), vps);
    ASSERT_THROWS([&] { expr->evaluate({}, &expCtx->variables); }(), AssertionException);
}

TEST(ExpressionPowTest, LargeExponentValuesWithBaseOfOne) {
    assertExpectedResults(
        "$pow",
        {
            {{Value(1), Value(10)}, Value(1)},
            {{Value(1), Value(10LL)}, Value(1LL)},
            {{Value(1), Value(10000LL)}, Value(1LL)},

            {{Value(1LL), Value(10LL)}, Value(1LL)},

            // $pow may sometimes use a loop to compute a^b, so it's important to check
            // that the loop doesn't hang if a large exponent is provided.
            {{Value(1LL), Value(std::numeric_limits<long long>::max())}, Value(1LL)},
            {{Value(1LL), Value(std::numeric_limits<long long>::min())}, Value(1LL)},
        });
}

TEST(ExpressionPowTest, LargeExponentValuesWithBaseOfNegativeOne) {
    assertExpectedResults("$pow",
                          {
                              {{Value(-1), Value(-1)}, Value(-1)},
                              {{Value(-1), Value(-2)}, Value(1)},
                              {{Value(-1), Value(-3)}, Value(-1)},

                              {{Value(-1LL), Value(0LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-1LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-2LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-3LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-4LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-5LL)}, Value(-1LL)},

                              {{Value(-1LL), Value(-61LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(61LL)}, Value(-1LL)},

                              {{Value(-1LL), Value(-62LL)}, Value(1LL)},
                              {{Value(-1LL), Value(62LL)}, Value(1LL)},

                              {{Value(-1LL), Value(-101LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-102LL)}, Value(1LL)},

                              // Use a value large enough that will make the test hang for a
                              // considerable amount of time if a loop is used to compute the
                              // answer.
                              {{Value(-1LL), Value(63234673905128LL)}, Value(1LL)},
                              {{Value(-1LL), Value(-63234673905128LL)}, Value(1LL)},

                              {{Value(-1LL), Value(63234673905127LL)}, Value(-1LL)},
                              {{Value(-1LL), Value(-63234673905127LL)}, Value(-1LL)},
                          });
}

TEST(ExpressionPowTest, LargeBaseSmallPositiveExponent) {
    assertExpectedResults("$pow",
                          {
                              {{Value(4294967296LL), Value(1LL)}, Value(4294967296LL)},
                              {{Value(4294967296LL), Value(0)}, Value(1LL)},
                          });
}

TEST(ExpressionArray, ExpressionArrayWithAllConstantValuesShouldOptimizeToExpressionConstant) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    // ExpressionArray of constant values should optimize to ExpressionConsant.
    BSONObj bsonarrayOfConstants = BSON("" << BSON_ARRAY(1 << 2 << 3 << 4));
    BSONElement elementArray = bsonarrayOfConstants.firstElement();
    auto expressionArr = ExpressionArray::parse(expCtx, elementArray, vps);
    auto optimizedToConstant = expressionArr->optimize();
    auto exprConstant = dynamic_cast<ExpressionConstant*>(optimizedToConstant.get());
    ASSERT_TRUE(exprConstant);

    // ExpressionArray with not all constant values should not optimize to ExpressionConstant.
    BSONObj bsonarray = BSON("" << BSON_ARRAY(1 << "$x" << 3 << 4));
    BSONElement elementArrayNotConstant = bsonarray.firstElement();
    auto expressionArrNotConstant = ExpressionArray::parse(expCtx, elementArrayNotConstant, vps);
    auto notOptimized = expressionArrNotConstant->optimize();
    auto notExprConstant = dynamic_cast<ExpressionConstant*>(notOptimized.get());
    ASSERT_FALSE(notExprConstant);
}

TEST(ExpressionArray, ExpressionArrayShouldOptimizeSubExpressionToExpressionConstant) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;


    // ExpressionArray with constant values and sub expression that evaluates to constant should
    // optimize to Expression constant.
    BSONObj bsonarrayWithSubExpression =
        BSON("" << BSON_ARRAY(1 << BSON("$add" << BSON_ARRAY(1 << 1)) << 3 << 4));
    BSONElement elementArrayWithSubExpression = bsonarrayWithSubExpression.firstElement();
    auto expressionArrWithSubExpression =
        ExpressionArray::parse(expCtx, elementArrayWithSubExpression, vps);
    auto optimizedToConstantWithSubExpression = expressionArrWithSubExpression->optimize();
    auto constantExpression =
        dynamic_cast<ExpressionConstant*>(optimizedToConstantWithSubExpression.get());
    ASSERT_TRUE(constantExpression);
}

TEST(ExpressionIndexOfArray, ExpressionIndexOfArrayShouldOptimizeArguments) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto expIndexOfArray = Expression::parseExpression(
        expCtx,  // 2, 1, 1
        BSON("$indexOfArray" << BSON_ARRAY(
                 BSON_ARRAY(BSON("$add" << BSON_ARRAY(1 << 1)) << 1 << 1 << 2)
                 // Value we are searching for = 2.
                 << BSON("$add" << BSON_ARRAY(1 << 1))
                 // Start index = 1.
                 << BSON("$add" << BSON_ARRAY(0 << 1))
                 // End index = 4.
                 << BSON("$add" << BSON_ARRAY(1 << 3)))),
        expCtx->variablesParseState);
    auto argsOptimizedToConstants = expIndexOfArray->optimize();
    auto shouldBeIndexOfArray = dynamic_cast<ExpressionConstant*>(argsOptimizedToConstants.get());
    ASSERT_TRUE(shouldBeIndexOfArray);
    ASSERT_VALUE_EQ(Value(3), shouldBeIndexOfArray->getValue());
}

TEST(ExpressionIndexOfArray,
     ExpressionIndexOfArrayShouldOptimizeNullishInputArrayToExpressionConstant) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    auto expIndex = Expression::parseExpression(
        expCtx, fromjson("{ $indexOfArray : [ undefined , 1, 1]}"), expCtx->variablesParseState);

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

    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto expIndexOfArray = Expression::parseExpression(
        expCtx,
        // Search for $x.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 3, 4, 5, 'val'] , '$x'] }"),
        expCtx->variablesParseState);
    auto optimizedIndexOfArray = expIndexOfArray->optimize();
    ASSERT_VALUE_EQ(Value(0),
                    optimizedIndexOfArray->evaluate(Document{{"x", 0}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(1),
                    optimizedIndexOfArray->evaluate(Document{{"x", 1}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(2),
                    optimizedIndexOfArray->evaluate(Document{{"x", 2}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(3),
                    optimizedIndexOfArray->evaluate(Document{{"x", 3}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(4),
                    optimizedIndexOfArray->evaluate(Document{{"x", 4}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(5),
                    optimizedIndexOfArray->evaluate(Document{{"x", 5}}, &expCtx->variables));
    ASSERT_VALUE_EQ(
        Value(6),
        optimizedIndexOfArray->evaluate(Document{{"x", string("val")}}, &expCtx->variables));

    auto optimizedIndexNotFound = optimizedIndexOfArray->optimize();
    // Should evaluate to -1 if not found.
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", 10}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", 100}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", 1000}}, &expCtx->variables));
    ASSERT_VALUE_EQ(
        Value(-1),
        optimizedIndexNotFound->evaluate(Document{{"x", string("string")}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexNotFound->evaluate(Document{{"x", -1}}, &expCtx->variables));
}

TEST(ExpressionIndexOfArray,
     OptimizedExpressionIndexOfArrayWithConstantArgumentsShouldEvaluateProperlyWithRange) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto expIndexOfArray = Expression::parseExpression(
        expCtx,
        // Search for 4 between 3 and 5.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 3, 4, 5] , '$x', 3, 5] }"),
        expCtx->variablesParseState);
    auto optimizedIndexOfArray = expIndexOfArray->optimize();
    ASSERT_VALUE_EQ(Value(4),
                    optimizedIndexOfArray->evaluate(Document{{"x", 4}}, &expCtx->variables));

    // Should evaluate to -1 if not found in range.
    ASSERT_VALUE_EQ(Value(-1),
                    optimizedIndexOfArray->evaluate(Document{{"x", 0}}, &expCtx->variables));
}

TEST(ExpressionIndexOfArray,
     OptimizedExpressionIndexOfArrayWithConstantArrayShouldEvaluateProperlyWithDuplicateValues) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto expIndexOfArrayWithDuplicateValues =
        Expression::parseExpression(expCtx,
                                    // Search for 4 between 3 and 5.
                                    fromjson("{ $indexOfArray : [ [0, 1, 2, 2, 3, 4, 5] , '$x'] }"),
                                    expCtx->variablesParseState);
    auto optimizedIndexOfArrayWithDuplicateValues = expIndexOfArrayWithDuplicateValues->optimize();
    ASSERT_VALUE_EQ(
        Value(2),
        optimizedIndexOfArrayWithDuplicateValues->evaluate(Document{{"x", 2}}, &expCtx->variables));
    // Duplicate Values in a range.
    auto expIndexInRangeWithhDuplicateValues = Expression::parseExpression(
        expCtx,
        // Search for 2 between 4 and 6.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 2, 2, 2, 4, 5] , '$x', 4, 6] }"),
        expCtx->variablesParseState);
    auto optimizedIndexInRangeWithDuplcateValues = expIndexInRangeWithhDuplicateValues->optimize();
    // Should evaluate to 4.
    ASSERT_VALUE_EQ(
        Value(4),
        optimizedIndexInRangeWithDuplcateValues->evaluate(Document{{"x", 2}}, &expCtx->variables));
}

namespace Parse {

namespace Object {

/**
 * Parses the object given by 'specification', with the options given by 'parseContextOptions'.
 */
boost::intrusive_ptr<Expression> parseObject(BSONObj specification) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    return Expression::parseObject(expCtx, specification, vps);
};

TEST(ParseObject, ShouldAcceptEmptyObject) {
    auto resultExpression = parseObject(BSONObj());

    // Should return an empty ExpressionObject.
    auto resultObject = dynamic_cast<ExpressionObject*>(resultExpression.get());
    ASSERT_TRUE(resultObject);

    ASSERT_EQ(resultObject->getChildExpressions().size(), 0UL);
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
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    return Expression::parseExpression(expCtx, specification, vps);
}

TEST(ParseExpression, ShouldRecognizeConstExpression) {
    auto resultExpression = parseExpression(BSON("$const" << 5));
    auto constExpression = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(constExpression);
    ASSERT_VALUE_EQ(constExpression->serialize(false), Value(Document{{"$const", 5}}));
}

TEST(ParseExpression, ShouldRejectUnknownExpression) {
    ASSERT_THROWS(parseExpression(BSON("$invalid" << 1)), AssertionException);
}

TEST(ParseExpression, ShouldRejectExpressionArgumentsWhichAreNotInArray) {
    ASSERT_THROWS(parseExpression(BSON("$strcasecmp"
                                       << "foo")),
                  AssertionException);
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
    auto resultExpression = parseExpression(BSON("$strcasecmp" << BSON_ARRAY("foo"
                                                                             << "FOO")));
    auto strCaseCmpExpression = dynamic_cast<ExpressionStrcasecmp*>(resultExpression.get());
    ASSERT_TRUE(strCaseCmpExpression);
    vector<Value> arguments = {Value(Document{{"$const", "foo"_sd}}),
                               Value(Document{{"$const", "FOO"_sd}})};
    ASSERT_VALUE_EQ(strCaseCmpExpression->serialize(false),
                    Value(Document{{"$strcasecmp", arguments}}));
}

TEST(ParseExpression, ShouldParseExpressionWithNoArguments) {
    auto resultExpression = parseExpression(BSON("$and" << BSONArray()));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    ASSERT_VALUE_EQ(andExpression->serialize(false), Value(Document{{"$and", vector<Value>{}}}));
}

TEST(ParseExpression, ShouldParseExpressionWithOneArgument) {
    auto resultExpression = parseExpression(BSON("$and" << BSON_ARRAY(1)));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(false), Value(Document{{"$and", arguments}}));
}

TEST(ParseExpression, ShouldAcceptArgumentWithoutArrayForVariadicExpressions) {
    auto resultExpression = parseExpression(BSON("$and" << 1));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(false), Value(Document{{"$and", arguments}}));
}

TEST(ParseExpression, ShouldAcceptArgumentWithoutArrayAsSingleArgument) {
    auto resultExpression = parseExpression(BSON("$not" << 1));
    auto notExpression = dynamic_cast<ExpressionNot*>(resultExpression.get());
    ASSERT_TRUE(notExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(notExpression->serialize(false), Value(Document{{"$not", arguments}}));
}

TEST(ParseExpression, ShouldAcceptObjectAsSingleArgument) {
    auto resultExpression = parseExpression(BSON("$and" << BSON("$const" << 1)));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(false), Value(Document{{"$and", arguments}}));
}

TEST(ParseExpression, ShouldAcceptObjectInsideArrayAsSingleArgument) {
    auto resultExpression = parseExpression(BSON("$and" << BSON_ARRAY(BSON("$const" << 1))));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    vector<Value> arguments = {Value(Document{{"$const", 1}})};
    ASSERT_VALUE_EQ(andExpression->serialize(false), Value(Document{{"$and", arguments}}));
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
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONElement specElement = specification.firstElement();
    VariablesParseState vps = expCtx->variablesParseState;
    return Expression::parseOperand(expCtx, specElement, vps);
}

TEST(ParseOperand, ShouldRecognizeFieldPath) {
    auto resultExpression = parseOperand(BSON(""
                                              << "$field"));
    auto fieldPathExpression = dynamic_cast<ExpressionFieldPath*>(resultExpression.get());
    ASSERT_TRUE(fieldPathExpression);
    ASSERT_VALUE_EQ(fieldPathExpression->serialize(false), Value("$field"_sd));
}

TEST(ParseOperand, ShouldRecognizeStringLiteral) {
    auto resultExpression = parseOperand(BSON(""
                                              << "foo"));
    auto constantExpression = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(constantExpression);
    ASSERT_VALUE_EQ(constantExpression->serialize(false), Value(Document{{"$const", "foo"_sd}}));
}

TEST(ParseOperand, ShouldRecognizeNestedArray) {
    auto resultExpression = parseOperand(BSON("" << BSON_ARRAY("foo"
                                                               << "$field")));
    auto arrayExpression = dynamic_cast<ExpressionArray*>(resultExpression.get());
    ASSERT_TRUE(arrayExpression);
    vector<Value> expectedSerializedArray = {Value(Document{{"$const", "foo"_sd}}),
                                             Value("$field"_sd)};
    ASSERT_VALUE_EQ(arrayExpression->serialize(false), Value(expectedSerializedArray));
}

TEST(ParseOperand, ShouldRecognizeNumberLiteral) {
    auto resultExpression = parseOperand(BSON("" << 5));
    auto constantExpression = dynamic_cast<ExpressionConstant*>(resultExpression.get());
    ASSERT_TRUE(constantExpression);
    ASSERT_VALUE_EQ(constantExpression->serialize(false), Value(Document{{"$const", 5}}));
}

TEST(ParseOperand, ShouldRecognizeNestedExpression) {
    auto resultExpression = parseOperand(BSON("" << BSON("$and" << BSONArray())));
    auto andExpression = dynamic_cast<ExpressionAnd*>(resultExpression.get());
    ASSERT_TRUE(andExpression);
    ASSERT_VALUE_EQ(andExpression->serialize(false), Value(Document{{"$and", vector<Value>{}}}));
}

}  // namespace Operand

}  // namespace Parse

namespace Set {
Value sortSet(Value set) {
    if (set.nullish()) {
        return Value(BSONNULL);
    }
    vector<Value> sortedSet = set.getArray();
    ValueComparator valueComparator;
    sort(sortedSet.begin(), sortedSet.end(), valueComparator.getLessThan());
    return Value(sortedSet);
}

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        const Document spec = getSpec();
        const Value args = spec["input"];
        if (!spec["expected"].missing()) {
            FieldIterator fields(spec["expected"].getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value expected = field.second;
                const BSONObj obj = BSON(field.first << args);
                VariablesParseState vps = expCtx->variablesParseState;
                const intrusive_ptr<Expression> expr =
                    Expression::parseExpression(expCtx, obj, vps);
                Value result = expr->evaluate({}, &expCtx->variables);
                if (result.getType() == Array) {
                    result = sortSet(result);
                }
                if (ValueComparator().evaluate(result != expected)) {
                    string errMsg = str::stream()
                        << "for expression " << field.first.toString() << " with argument "
                        << args.toString() << " full tree: " << expr->serialize(false).toString()
                        << " expected: " << expected.toString()
                        << " but got: " << result.toString();
                    FAIL(errMsg);
                }
                // TODO test optimize here
            }
        }
        if (!spec["error"].missing()) {
            const vector<Value>& asserters = spec["error"].getArray();
            size_t n = asserters.size();
            for (size_t i = 0; i < n; i++) {
                const BSONObj obj = BSON(asserters[i].getString() << args);
                VariablesParseState vps = expCtx->variablesParseState;
                ASSERT_THROWS(
                    [&] {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(expCtx, obj, vps);
                        expr->evaluate({}, &expCtx->variables);
                    }(),
                    AssertionException);
            }
        }
    }

private:
    virtual Document getSpec() = 0;
};

class Same : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection" << DOC_ARRAY(1 << 2)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2)
                                                 << "$setDifference" << vector<Value>()));
    }
};

class Redundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection" << DOC_ARRAY(1 << 2)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2)
                                                 << "$setDifference" << vector<Value>()));
    }
};

class DoubleRedundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(DOC_ARRAY(1 << 1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << vector<Value>()));
    }
};

class Super : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection" << DOC_ARRAY(1)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2)
                                                 << "$setDifference" << DOC_ARRAY(2)));
    }
};

class SuperWithRedundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 2) << DOC_ARRAY(1)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection" << DOC_ARRAY(1)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2)
                                                 << "$setDifference" << DOC_ARRAY(2)));
    }
};

class Sub : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << false
                                                 << "$setIntersection" << DOC_ARRAY(1)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2)
                                                 << "$setDifference" << vector<Value>()));
    }
};

class SameBackwards : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(2 << 1)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection" << DOC_ARRAY(1 << 2)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2)
                                                 << "$setDifference" << vector<Value>()));
    }
};

class NoOverlap : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 4)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection" << vector<Value>()
                                                 << "$setUnion" << DOC_ARRAY(1 << 2 << 4 << 8)
                                                 << "$setDifference" << DOC_ARRAY(1 << 2)));
    }
};

class Overlap : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 2 << 4)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection" << DOC_ARRAY(2)
                                                 << "$setUnion" << DOC_ARRAY(1 << 2 << 4 << 8)
                                                 << "$setDifference" << DOC_ARRAY(1)));
    }
};

class LastNull : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << Value(BSONNULL)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference" << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"_sd
                                        << "$setIsSubset"_sd));
    }
};

class FirstNull : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(Value(BSONNULL) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference" << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"_sd
                                        << "$setIsSubset"_sd));
    }
};

class LeftNullAndRightEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(Value(BSONNULL) << vector<Value>()) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference" << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"_sd
                                        << "$setIsSubset"_sd));
    }
};

class RightNullAndLeftEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(vector<Value>() << Value(BSONNULL)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference" << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"_sd
                                        << "$setIsSubset"_sd));
    }
};

class NoArg : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << vector<Value>() << "expected"
                    << DOC("$setIntersection" << vector<Value>() << "$setUnion" << vector<Value>())
                    << "error"
                    << DOC_ARRAY("$setEquals"_sd
                                 << "$setIsSubset"_sd
                                 << "$setDifference"_sd));
    }
};

class OneArg : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << DOC_ARRAY(1 << 2) << "$setUnion"
                                                     << DOC_ARRAY(1 << 2))
                           << "error"
                           << DOC_ARRAY("$setEquals"_sd
                                        << "$setIsSubset"_sd
                                        << "$setDifference"_sd));
    }
};

class EmptyArg : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(vector<Value>()) << "expected"
                    << DOC("$setIntersection" << vector<Value>() << "$setUnion" << vector<Value>())
                    << "error"
                    << DOC_ARRAY("$setEquals"_sd
                                 << "$setIsSubset"_sd
                                 << "$setDifference"_sd));
    }
};

class LeftArgEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(vector<Value>() << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << vector<Value>() << "$setUnion"
                                                     << DOC_ARRAY(1 << 2) << "$setIsSubset" << true
                                                     << "$setEquals" << false << "$setDifference"
                                                     << vector<Value>()));
    }
};

class RightArgEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << vector<Value>()) << "expected"
                           << DOC("$setIntersection" << vector<Value>() << "$setUnion"
                                                     << DOC_ARRAY(1 << 2) << "$setIsSubset" << false
                                                     << "$setEquals" << false << "$setDifference"
                                                     << DOC_ARRAY(1 << 2)));
    }
};

class ManyArgs : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(8 << 3)
                                        << DOC_ARRAY("asdf"_sd
                                                     << "foo"_sd)
                                        << DOC_ARRAY(80.3 << 34) << vector<Value>()
                                        << DOC_ARRAY(80.3 << "foo"_sd << 11 << "yay"_sd))
                           << "expected"
                           << DOC("$setIntersection"
                                  << vector<Value>() << "$setEquals" << false << "$setUnion"
                                  << DOC_ARRAY(3 << 8 << 11 << 34 << 80.3 << "asdf"_sd
                                                 << "foo"_sd
                                                 << "yay"_sd))
                           << "error"
                           << DOC_ARRAY("$setIsSubset"_sd
                                        << "$setDifference"_sd));
    }
};

class ManyArgsEqual : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 4)
                                        << DOC_ARRAY(1 << 2 << 2 << 4) << DOC_ARRAY(4 << 1 << 2)
                                        << DOC_ARRAY(2 << 1 << 1 << 4))
                           << "expected"
                           << DOC("$setIntersection" << DOC_ARRAY(1 << 2 << 4) << "$setEquals"
                                                     << true << "$setUnion"
                                                     << DOC_ARRAY(1 << 2 << 4))
                           << "error"
                           << DOC_ARRAY("$setIsSubset"_sd
                                        << "$setDifference"_sd));
    }
};
}  // namespace Set

namespace Strcasecmp {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        assertResult(expectedResult(), spec());
        assertResult(-expectedResult(), reverseSpec());
    }

protected:
    virtual string a() = 0;
    virtual string b() = 0;
    virtual int expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$strcasecmp" << BSON_ARRAY(a() << b()));
    }
    BSONObj reverseSpec() {
        return BSON("$strcasecmp" << BSON_ARRAY(b() << a()));
    }
    void assertResult(int expectedResult, const BSONObj& spec) {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObj = BSON("" << spec);
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                          toBson(expression->evaluate({}, &expCtx->variables)));
    }
};

class NullBegin : public ExpectedResultBase {
    string a() {
        return string("\0ab", 3);
    }
    string b() {
        return string("\0AB", 3);
    }
    int expectedResult() {
        return 0;
    }
};

class NullEnd : public ExpectedResultBase {
    string a() {
        return string("ab\0", 3);
    }
    string b() {
        return string("aB\0", 3);
    }
    int expectedResult() {
        return 0;
    }
};

class NullMiddleLt : public ExpectedResultBase {
    string a() {
        return string("a\0a", 3);
    }
    string b() {
        return string("a\0B", 3);
    }
    int expectedResult() {
        return -1;
    }
};

class NullMiddleEq : public ExpectedResultBase {
    string a() {
        return string("a\0b", 3);
    }
    string b() {
        return string("a\0B", 3);
    }
    int expectedResult() {
        return 0;
    }
};

class NullMiddleGt : public ExpectedResultBase {
    string a() {
        return string("a\0c", 3);
    }
    string b() {
        return string("a\0B", 3);
    }
    int expectedResult() {
        return 1;
    }
};

}  // namespace Strcasecmp

namespace StrLenBytes {

TEST(ExpressionStrLenBytes, ComputesLengthOfString) {
    assertExpectedResults("$strLenBytes", {{{Value("abc"_sd)}, Value(3)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenBytes", {{{Value(StringData())}, Value(0)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenBytes", {{{Value("ab\0c"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenBytes", {{{Value("abc\0"_sd)}, Value(4)}});
}

}  // namespace StrLenBytes

namespace StrLenCP {

TEST(ExpressionStrLenCP, ComputesLengthOfASCIIString) {
    assertExpectedResults("$strLenCP", {{{Value("abc"_sd)}, Value(3)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenCP", {{{Value(StringData())}, Value(0)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenCP", {{{Value("ab\0c"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenCP", {{{Value("abc\0"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithAccent) {
    assertExpectedResults("$strLenCP", {{{Value("a\0bâ"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithSpecialCharacters) {
    assertExpectedResults("$strLenCP", {{{Value("ºabøåß"_sd)}, Value(6)}});
}

}  // namespace StrLenCP

namespace SubstrBytes {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate({}, &expCtx->variables)));
    }

protected:
    virtual string str() = 0;
    virtual int offset() = 0;
    virtual int length() = 0;
    virtual string expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$substrBytes" << BSON_ARRAY(str() << offset() << length()));
    }
};

/** Retrieve a full string containing a null character. */
class FullNull : public ExpectedResultBase {
    string str() {
        return string("a\0b", 3);
    }
    int offset() {
        return 0;
    }
    int length() {
        return 3;
    }
    string expectedResult() {
        return str();
    }
};

/** Retrieve a substring beginning with a null character. */
class BeginAtNull : public ExpectedResultBase {
    string str() {
        return string("a\0b", 3);
    }
    int offset() {
        return 1;
    }
    int length() {
        return 2;
    }
    string expectedResult() {
        return string("\0b", 2);
    }
};

/** Retrieve a substring ending with a null character. */
class EndAtNull : public ExpectedResultBase {
    string str() {
        return string("a\0b", 3);
    }
    int offset() {
        return 0;
    }
    int length() {
        return 2;
    }
    string expectedResult() {
        return string("a\0", 2);
    }
};

/** Drop a beginning null character. */
class DropBeginningNull : public ExpectedResultBase {
    string str() {
        return string("\0b", 2);
    }
    int offset() {
        return 1;
    }
    int length() {
        return 1;
    }
    string expectedResult() {
        return "b";
    }
};

/** Drop an ending null character. */
class DropEndingNull : public ExpectedResultBase {
    string str() {
        return string("a\0", 2);
    }
    int offset() {
        return 0;
    }
    int length() {
        return 1;
    }
    string expectedResult() {
        return "a";
    }
};

/** When length is negative, the remainder of the string should be returned. */
class NegativeLength : public ExpectedResultBase {
    string str() {
        return string("abcdefghij");
    }
    int offset() {
        return 2;
    }
    int length() {
        return -1;
    }
    string expectedResult() {
        return "cdefghij";
    }
};

TEST(ExpressionSubstrTest, ThrowsWithNegativeStart) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    const auto str = "abcdef"_sd;
    const auto expr =
        Expression::parseExpression(expCtx, BSON("$substrCP" << BSON_ARRAY(str << -5 << 1)), vps);
    ASSERT_THROWS([&] { expr->evaluate({}, &expCtx->variables); }(), AssertionException);
}

}  // namespace SubstrBytes

namespace SubstrCP {

TEST(ExpressionSubstrCPTest, DoesThrowWithBadContinuationByte) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    const auto continuationByte = "\x80\x00"_sd;
    const auto expr = Expression::parseExpression(
        expCtx, BSON("$substrCP" << BSON_ARRAY(continuationByte << 0 << 1)), vps);
    ASSERT_THROWS([&] { expr->evaluate({}, &expCtx->variables); }(), AssertionException);
}

TEST(ExpressionSubstrCPTest, DoesThrowWithInvalidLeadingByte) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    const auto leadingByte = "\xFF\x00"_sd;
    const auto expr = Expression::parseExpression(
        expCtx, BSON("$substrCP" << BSON_ARRAY(leadingByte << 0 << 1)), vps);
    ASSERT_THROWS([&] { expr->evaluate({}, &expCtx->variables); }(), AssertionException);
}

TEST(ExpressionSubstrCPTest, WithStandardValue) {
    assertExpectedResults("$substrCP", {{{Value("abc"_sd), Value(0), Value(2)}, Value("ab"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacter) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc\0d"_sd), Value(2), Value(3)}, Value("c\0d"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacterAtEnd) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc\0"_sd), Value(2), Value(2)}, Value("c\0"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithOutOfRangeString) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc"_sd), Value(3), Value(2)}, Value(StringData())}});
}

TEST(ExpressionSubstrCPTest, WithPartiallyOutOfRangeString) {
    assertExpectedResults("$substrCP", {{{Value("abc"_sd), Value(1), Value(4)}, Value("bc"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithUnicodeValue) {
    assertExpectedResults("$substrCP",
                          {{{Value("øø∫å"_sd), Value(0), Value(4)}, Value("øø∫å"_sd)}});
    assertExpectedResults("$substrBytes",
                          {{{Value("øø∫å"_sd), Value(0), Value(4)}, Value("øø"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithMixedUnicodeAndASCIIValue) {
    assertExpectedResults("$substrCP",
                          {{{Value("a∫bøßabc"_sd), Value(1), Value(4)}, Value("∫bøß"_sd)}});
    assertExpectedResults("$substrBytes",
                          {{{Value("a∫bøßabc"_sd), Value(1), Value(4)}, Value("∫b"_sd)}});
}

TEST(ExpressionSubstrCPTest, ShouldCoerceDateToString) {
    assertExpectedResults("$substrCP",
                          {{{Value(Date_t::fromMillisSinceEpoch(0)), Value(0), Value(1000)},
                            Value("1970-01-01T00:00:00.000Z"_sd)}});
    assertExpectedResults("$substrBytes",
                          {{{Value(Date_t::fromMillisSinceEpoch(0)), Value(0), Value(1000)},
                            Value("1970-01-01T00:00:00.000Z"_sd)}});
}

}  // namespace SubstrCP

namespace Type {

TEST(ExpressionTypeTest, WithMinKeyValue) {
    assertExpectedResults("$type", {{{Value(MINKEY)}, Value("minKey"_sd)}});
}

TEST(ExpressionTypeTest, WithDoubleValue) {
    assertExpectedResults("$type", {{{Value(1.0)}, Value("double"_sd)}});
}

TEST(ExpressionTypeTest, WithStringValue) {
    assertExpectedResults("$type", {{{Value("stringValue"_sd)}, Value("string"_sd)}});
}

TEST(ExpressionTypeTest, WithObjectValue) {
    BSONObj objectVal = fromjson("{a: {$literal: 1}}");
    assertExpectedResults("$type", {{{Value(objectVal)}, Value("object"_sd)}});
}

TEST(ExpressionTypeTest, WithArrayValue) {
    assertExpectedResults("$type", {{{Value(BSON_ARRAY(1 << 2))}, Value("array"_sd)}});
}

TEST(ExpressionTypeTest, WithBinDataValue) {
    BSONBinData binDataVal = BSONBinData("", 0, BinDataGeneral);
    assertExpectedResults("$type", {{{Value(binDataVal)}, Value("binData"_sd)}});
}

TEST(ExpressionTypeTest, WithUndefinedValue) {
    assertExpectedResults("$type", {{{Value(BSONUndefined)}, Value("undefined"_sd)}});
}

TEST(ExpressionTypeTest, WithOIDValue) {
    assertExpectedResults("$type", {{{Value(OID())}, Value("objectId"_sd)}});
}

TEST(ExpressionTypeTest, WithBoolValue) {
    assertExpectedResults("$type", {{{Value(true)}, Value("bool"_sd)}});
}

TEST(ExpressionTypeTest, WithDateValue) {
    Date_t dateVal = BSON("" << DATENOW).firstElement().Date();
    assertExpectedResults("$type", {{{Value(dateVal)}, Value("date"_sd)}});
}

TEST(ExpressionTypeTest, WithNullValue) {
    assertExpectedResults("$type", {{{Value(BSONNULL)}, Value("null"_sd)}});
}

TEST(ExpressionTypeTest, WithRegexValue) {
    assertExpectedResults("$type", {{{Value(BSONRegEx("a.b"))}, Value("regex"_sd)}});
}

TEST(ExpressionTypeTest, WithSymbolValue) {
    assertExpectedResults("$type", {{{Value(BSONSymbol("a"))}, Value("symbol"_sd)}});
}

TEST(ExpressionTypeTest, WithDBRefValue) {
    assertExpectedResults("$type", {{{Value(BSONDBRef("", OID()))}, Value("dbPointer"_sd)}});
}

TEST(ExpressionTypeTest, WithCodeWScopeValue) {
    assertExpectedResults(
        "$type",
        {{{Value(BSONCodeWScope("var x = 3", BSONObj()))}, Value("javascriptWithScope"_sd)}});
}

TEST(ExpressionTypeTest, WithCodeValue) {
    assertExpectedResults("$type", {{{Value(BSONCode("var x = 3"))}, Value("javascript"_sd)}});
}

TEST(ExpressionTypeTest, WithIntValue) {
    assertExpectedResults("$type", {{{Value(1)}, Value("int"_sd)}});
}

TEST(ExpressionTypeTest, WithDecimalValue) {
    assertExpectedResults("$type", {{{Value(Decimal128(0.3))}, Value("decimal"_sd)}});
}

TEST(ExpressionTypeTest, WithLongValue) {
    assertExpectedResults("$type", {{{Value(1LL)}, Value("long"_sd)}});
}

TEST(ExpressionTypeTest, WithTimestampValue) {
    assertExpectedResults("$type", {{{Value(Timestamp(0, 0))}, Value("timestamp"_sd)}});
}

TEST(ExpressionTypeTest, WithMaxKeyValue) {
    assertExpectedResults("$type", {{{Value(MAXKEY)}, Value("maxKey"_sd)}});
}

}  // namespace Type

namespace IsNumber {

TEST(ExpressionIsNumberTest, WithMinKeyValue) {
    assertExpectedResults("$isNumber", {{{Value(MINKEY)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithDoubleValue) {
    assertExpectedResults("$isNumber", {{{Value(1.0)}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithStringValue) {
    assertExpectedResults("$isNumber", {{{Value("stringValue"_sd)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithNumericStringValue) {
    assertExpectedResults("$isNumber", {{{Value("5"_sd)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithObjectValue) {
    BSONObj objectVal = fromjson("{a: {$literal: 1}}");
    assertExpectedResults("$isNumber", {{{Value(objectVal)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithArrayValue) {
    assertExpectedResults("$isNumber", {{{Value(BSON_ARRAY(1 << 2))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithBinDataValue) {
    BSONBinData binDataVal = BSONBinData("", 0, BinDataGeneral);
    assertExpectedResults("$isNumber", {{{Value(binDataVal)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithUndefinedValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONUndefined)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithOIDValue) {
    assertExpectedResults("$isNumber", {{{Value(OID())}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithBoolValue) {
    assertExpectedResults("$isNumber", {{{Value(true)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithDateValue) {
    Date_t dateVal = BSON("" << DATENOW).firstElement().Date();
    assertExpectedResults("$isNumber", {{{Value(dateVal)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithNullValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONNULL)}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithRegexValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONRegEx("a.b"))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithSymbolValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONSymbol("a"))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithDBRefValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONDBRef("", OID()))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithCodeWScopeValue) {
    assertExpectedResults("$isNumber",
                          {{{Value(BSONCodeWScope("var x = 3", BSONObj()))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithCodeValue) {
    assertExpectedResults("$isNumber", {{{Value(BSONCode("var x = 3"))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithIntValue) {
    assertExpectedResults("$isNumber", {{{Value(1)}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithDecimalValue) {
    assertExpectedResults("$isNumber", {{{Value(Decimal128(0.3))}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithLongValue) {
    assertExpectedResults("$isNumber", {{{Value(1LL)}, Value(true)}});
}

TEST(ExpressionIsNumberTest, WithTimestampValue) {
    assertExpectedResults("$isNumber", {{{Value(Timestamp(0, 0))}, Value(false)}});
}

TEST(ExpressionIsNumberTest, WithMaxKeyValue) {
    assertExpectedResults("$isNumber", {{{Value(MAXKEY)}, Value(false)}});
}

}  // namespace IsNumber

namespace BuiltinRemoveVariable {

TEST(BuiltinRemoveVariableTest, TypeOfRemoveIsMissing) {
    assertExpectedResults("$type", {{{Value("$$REMOVE"_sd)}, Value("missing"_sd)}});
}

TEST(BuiltinRemoveVariableTest, LiteralEscapesRemoveVar) {
    assertExpectedResults(
        "$literal", {{{Value("$$REMOVE"_sd)}, Value(std::vector<Value>{Value("$$REMOVE"_sd)})}});
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectly) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto expression = ExpressionFieldPath::parse(expCtx, "$$REMOVE", vps);
    ASSERT_BSONOBJ_EQ(BSON("foo"
                           << "$$REMOVE"),
                      BSON("foo" << expression->serialize(false)));
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectlyWithTrailingPath) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto expression = ExpressionFieldPath::parse(expCtx, "$$REMOVE.a.b", vps);
    ASSERT_BSONOBJ_EQ(BSON("foo"
                           << "$$REMOVE.a.b"),
                      BSON("foo" << expression->serialize(false)));
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectlyAfterOptimization) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto expression = ExpressionFieldPath::parse(expCtx, "$$REMOVE.a.b", vps);
    auto optimizedExpression = expression->optimize();
    ASSERT(dynamic_cast<ExpressionConstant*>(optimizedExpression.get()));
    ASSERT_BSONOBJ_EQ(BSON("foo"
                           << "$$REMOVE"),
                      BSON("foo" << optimizedExpression->serialize(false)));
}

}  // namespace BuiltinRemoveVariable

/* ------------------------- ExpressionMergeObjects -------------------------- */

namespace ExpressionMergeObjects {

TEST(ExpressionMergeObjects, MergingWithSingleObjectShouldLeaveUnchanged) {
    assertExpectedResults("$mergeObjects", {{{}, {Document({})}}});

    auto doc = Document({{"a", 1}, {"b", 1}});
    assertExpectedResults("$mergeObjects", {{{doc}, doc}});
}

TEST(ExpressionMergeObjects, MergingDisjointObjectsShouldIncludeAllFields) {
    auto first = Document({{"a", 1}, {"b", 1}});
    auto second = Document({{"c", 1}});
    assertExpectedResults("$mergeObjects",
                          {{{first, second}, Document({{"a", 1}, {"b", 1}, {"c", 1}})}});
}

TEST(ExpressionMergeObjects, MergingIntersectingObjectsShouldOverrideInOrderReceived) {
    auto first = Document({{"a", "oldValue"_sd}, {"b", 0}, {"c", 1}});
    auto second = Document({{"a", "newValue"_sd}});
    assertExpectedResults(
        "$mergeObjects", {{{first, second}, Document({{"a", "newValue"_sd}, {"b", 0}, {"c", 1}})}});
}

TEST(ExpressionMergeObjects, MergingIntersectingEmbeddedObjectsShouldOverrideInOrderReceived) {
    auto firstSubDoc = Document({{"a", 1}, {"b", 2}, {"c", 3}});
    auto secondSubDoc = Document({{"a", 2}, {"b", 1}});
    auto first = Document({{"d", 1}, {"subDoc", firstSubDoc}});
    auto second = Document({{"subDoc", secondSubDoc}});
    auto expected = Document({{"d", 1}, {"subDoc", secondSubDoc}});
    assertExpectedResults("$mergeObjects", {{{first, second}, expected}});
}

TEST(ExpressionMergeObjects, MergingWithEmptyDocumentShouldIgnore) {
    auto first = Document({{"a", 0}, {"b", 1}, {"c", 1}});
    auto second = Document({});
    auto expected = Document({{"a", 0}, {"b", 1}, {"c", 1}});
    assertExpectedResults("$mergeObjects", {{{first, second}, expected}});
}

TEST(ExpressionMergeObjects, MergingSingleArgumentArrayShouldUnwindAndMerge) {
    std::vector<Document> first = {Document({{"a", 1}}), Document({{"a", 2}})};
    auto expected = Document({{"a", 2}});
    assertExpectedResults("$mergeObjects", {{{first}, expected}});
}

TEST(ExpressionMergeObjects, MergingArrayWithDocumentShouldThrowException) {
    std::vector<Document> first = {Document({{"a", 1}}), Document({{"a", 2}})};
    auto second = Document({{"b", 2}});
    ASSERT_THROWS_CODE(
        evaluateExpression("$mergeObjects", {first, second}), AssertionException, 40400);
}

TEST(ExpressionMergeObjects, MergingArrayContainingInvalidTypesShouldThrowException) {
    std::vector<Value> first = {Value(Document({{"validType", 1}})), Value("invalidType"_sd)};
    ASSERT_THROWS_CODE(evaluateExpression("$mergeObjects", {first}), AssertionException, 40400);
}

TEST(ExpressionMergeObjects, MergingNonObjectsShouldThrowException) {
    ASSERT_THROWS_CODE(
        evaluateExpression("$mergeObjects", {"invalidArg"_sd}), AssertionException, 40400);

    ASSERT_THROWS_CODE(
        evaluateExpression("$mergeObjects", {"invalidArg"_sd, Document({{"validArg", 1}})}),
        AssertionException,
        40400);

    ASSERT_THROWS_CODE(evaluateExpression("$mergeObjects", {1, Document({{"validArg", 1}})}),
                       AssertionException,
                       40400);
}

}  // namespace ExpressionMergeObjects


namespace ToLower {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate({}, &expCtx->variables)));
    }

protected:
    virtual string str() = 0;
    virtual string expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$toLower" << BSON_ARRAY(str()));
    }
};

/** String beginning with a null character. */
class NullBegin : public ExpectedResultBase {
    string str() {
        return string("\0aB", 3);
    }
    string expectedResult() {
        return string("\0ab", 3);
    }
};

/** String containing a null character. */
class NullMiddle : public ExpectedResultBase {
    string str() {
        return string("a\0B", 3);
    }
    string expectedResult() {
        return string("a\0b", 3);
    }
};

/** String ending with a null character. */
class NullEnd : public ExpectedResultBase {
    string str() {
        return string("aB\0", 3);
    }
    string expectedResult() {
        return string("ab\0", 3);
    }
};

}  // namespace ToLower

namespace ToUpper {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate({}, &expCtx->variables)));
    }

protected:
    virtual string str() = 0;
    virtual string expectedResult() = 0;

private:
    BSONObj spec() {
        return BSON("$toUpper" << BSON_ARRAY(str()));
    }
};

/** String beginning with a null character. */
class NullBegin : public ExpectedResultBase {
    string str() {
        return string("\0aB", 3);
    }
    string expectedResult() {
        return string("\0AB", 3);
    }
};

/** String containing a null character. */
class NullMiddle : public ExpectedResultBase {
    string str() {
        return string("a\0B", 3);
    }
    string expectedResult() {
        return string("A\0B", 3);
    }
};

/** String ending with a null character. */
class NullEnd : public ExpectedResultBase {
    string str() {
        return string("aB\0", 3);
    }
    string expectedResult() {
        return string("AB\0", 3);
    }
};

}  // namespace ToUpper

namespace AllAnyElements {
class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        const Document spec = getSpec();
        const Value args = spec["input"];
        if (!spec["expected"].missing()) {
            FieldIterator fields(spec["expected"].getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value expected = field.second;
                const BSONObj obj = BSON(field.first << args);
                VariablesParseState vps = expCtx->variablesParseState;
                const intrusive_ptr<Expression> expr =
                    Expression::parseExpression(expCtx, obj, vps);
                const Value result = expr->evaluate({}, &expCtx->variables);
                if (ValueComparator().evaluate(result != expected)) {
                    string errMsg = str::stream()
                        << "for expression " << field.first.toString() << " with argument "
                        << args.toString() << " full tree: " << expr->serialize(false).toString()
                        << " expected: " << expected.toString()
                        << " but got: " << result.toString();
                    FAIL(errMsg);
                }
                // TODO test optimize here
            }
        }
        if (!spec["error"].missing()) {
            const vector<Value>& asserters = spec["error"].getArray();
            size_t n = asserters.size();
            for (size_t i = 0; i < n; i++) {
                const BSONObj obj = BSON(asserters[i].getString() << args);
                VariablesParseState vps = expCtx->variablesParseState;
                ASSERT_THROWS(
                    [&] {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(expCtx, obj, vps);
                        expr->evaluate({}, &expCtx->variables);
                    }(),
                    AssertionException);
            }
        }
    }

private:
    virtual Document getSpec() = 0;
};

class JustFalse : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(false)) << "expected"
                           << DOC("$allElementsTrue" << false << "$anyElementTrue" << false));
    }
};

class JustTrue : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(true)) << "expected"
                           << DOC("$allElementsTrue" << true << "$anyElementTrue" << true));
    }
};

class OneTrueOneFalse : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(true << false)) << "expected"
                           << DOC("$allElementsTrue" << false << "$anyElementTrue" << true));
    }
};

class Empty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(vector<Value>()) << "expected"
                           << DOC("$allElementsTrue" << true << "$anyElementTrue" << false));
    }
};

class TrueViaInt : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1)) << "expected"
                           << DOC("$allElementsTrue" << true << "$anyElementTrue" << true));
    }
};

class FalseViaInt : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(0)) << "expected"
                           << DOC("$allElementsTrue" << false << "$anyElementTrue" << false));
    }
};

class Null : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(BSONNULL) << "error"
                           << DOC_ARRAY("$allElementsTrue"_sd
                                        << "$anyElementTrue"_sd));
    }
};

}  // namespace AllAnyElements

namespace GetComputedPathsTest {

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWhenUsingRemoveBuiltin) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx, "$$REMOVE", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("a", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("a"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWhenOnlyRoot) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx, "$$ROOT", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("a", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("a"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWithNonMatchingUserVariable) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->variablesParseState.defineVariable("userVar");
    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar.b", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("a", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("a"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesNotCountAsRenameWhenDotted) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx, "$a.b", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("c", Variables::kRootId);
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("c"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRename) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx, "$a", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", Variables::kRootId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRenameWithExplicitRoot) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx, "$$ROOT.a", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", Variables::kRootId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRenameWithExplicitCurrent) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx, "$$CURRENT.a", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", Variables::kRootId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionFieldPathDoesCountAsRenameWithMatchingUserVariable) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar.a", expCtx->variablesParseState);
    auto computedPaths = expr->getComputedPaths("b", varId);
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["b"], "a");
}

TEST(GetComputedPathsTest, ExpressionObjectCorrectlyReportsComputedPaths) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson("{a: '$b', c: {$add: [1, 3]}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionObject*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("d");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("d.c"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["d.a"], "b");
}

TEST(GetComputedPathsTest, ExpressionObjectCorrectlyReportsComputedPathsNested) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson(
        "{a: {b: '$c'},"
        "d: {$map: {input: '$e', as: 'iter', in: {f: '$$iter.g'}}}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionObject*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("h");
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 2u);
    ASSERT_EQ(computedPaths.renames["h.a.b"], "c");
    ASSERT_EQ(computedPaths.renames["h.d.f"], "e.g");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPaths) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject =
        fromjson("{$map: {input: '$a', as: 'iter', in: {b: '$$iter.c', d: {$add: [1, 2]}}}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e.d"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["e.b"], "a.c");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPathsWithDefaultVarName) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson("{$map: {input: '$a', in: {b: '$$this.c', d: {$add: [1, 2]}}}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e.d"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["e.b"], "a.c");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPathsWithNestedExprObject) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson("{$map: {input: '$a', in: {b: {c: '$$this.d'}}}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["e.b.c"], "a.d");
}

TEST(GetComputedPathsTest, ExpressionMapNotConsideredRenameWithWrongRootVariable) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson("{$map: {input: '$a', as: 'iter', in: {b: '$c'}}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("d");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("d"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapNotConsideredRenameWithWrongVariableNoExpressionObject) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson("{$map: {input: '$a', as: 'iter', in: '$b'}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("d");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("d"), 1u);
    ASSERT(computedPaths.renames.empty());
}

TEST(GetComputedPathsTest, ExpressionMapNotConsideredRenameWithDottedInputPath) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto specObject = fromjson("{$map: {input: '$a.b', as: 'iter', in: {c: '$$iter.d'}}}}");
    auto expr = Expression::parseObject(expCtx, specObject, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e"), 1u);
    ASSERT(computedPaths.renames.empty());
}

}  // namespace GetComputedPathsTest

namespace expression_meta_test {
TEST(ExpressionMetaTest, ExpressionMetaSearchScore) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchScore\"}");
    auto expressionMeta = ExpressionMeta::parse(expCtx, expr.firstElement(), vps);

    MutableDocument doc;
    doc.metadata().setSearchScore(1.234);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_EQ(val.getDouble(), 1.234);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchHighlights) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchHighlights\"}");
    auto expressionMeta = ExpressionMeta::parse(expCtx, expr.firstElement(), vps);

    MutableDocument doc;
    Document highlights = DOC("this part" << 1 << "is opaque to the server" << 1);
    doc.metadata().setSearchHighlights(Value(highlights));

    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), highlights);
}

TEST(ExpressionMetaTest, ExpressionMetaGeoNearDistance) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"geoNearDistance\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    doc.metadata().setGeoNearDistance(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaGeoNearPoint) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"geoNearPoint\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    Document pointDoc = Document{fromjson("{some: 'document'}")};
    doc.metadata().setGeoNearPoint(Value(pointDoc));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), pointDoc);
}

TEST(ExpressionMetaTest, ExpressionMetaIndexKey) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"indexKey\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    BSONObj ixKey = fromjson("{'': 1, '': 'string'}");
    doc.metadata().setIndexKey(ixKey);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), Document(ixKey));
}

TEST(ExpressionMetaTest, ExpressionMetaRecordId) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"recordId\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    doc.metadata().setRecordId(RecordId(123LL));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_EQ(val.getLong(), 123LL);
}

TEST(ExpressionMetaTest, ExpressionMetaRandVal) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"randVal\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    doc.metadata().setRandVal(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaSortKey) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"sortKey\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    Value sortKey = Value(std::vector<Value>{Value(1), Value(2)});
    doc.metadata().setSortKey(sortKey, /* isSingleElementSortKey = */ false);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>{Value(1), Value(2)}));
}

TEST(ExpressionMetaTest, ExpressionMetaTextScore) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj expr = fromjson("{$meta: \"textScore\"}");
    auto expressionMeta =
        ExpressionMeta::parse(expCtx, expr.firstElement(), expCtx->variablesParseState);

    MutableDocument doc;
    doc.metadata().setTextScore(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx->variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}
}  // namespace expression_meta_test

namespace ExpressionRegexTest {

class ExpressionRegexTest {
public:
    template <typename ExpressionRegexSubClass>
    static intrusive_ptr<Expression> generateOptimizedExpression(
        const BSONObj& input, intrusive_ptr<ExpressionContextForTest> expCtx) {

        auto expression = ExpressionRegexSubClass::parse(
            expCtx, input.firstElement(), expCtx->variablesParseState);
        return expression->optimize();
    }

    static void testAllExpressions(const BSONObj& input,
                                   bool optimized,
                                   const std::vector<Value>& expectedFindAllOutput) {

        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        {
            // For $regexFindAll.
            auto expression = generateOptimizedExpression<ExpressionRegexFindAll>(input, expCtx);
            auto regexFindAllExpr = dynamic_cast<ExpressionRegexFindAll*>(expression.get());
            ASSERT_EQ(regexFindAllExpr->hasConstantRegex(), optimized);
            Value output = expression->evaluate({}, &expCtx->variables);
            ASSERT_VALUE_EQ(output, Value(expectedFindAllOutput));
        }
        {
            // For $regexFind.
            auto expression = generateOptimizedExpression<ExpressionRegexFind>(input, expCtx);
            auto regexFindExpr = dynamic_cast<ExpressionRegexFind*>(expression.get());
            ASSERT_EQ(regexFindExpr->hasConstantRegex(), optimized);
            Value output = expression->evaluate({}, &expCtx->variables);
            ASSERT_VALUE_EQ(
                output, expectedFindAllOutput.empty() ? Value(BSONNULL) : expectedFindAllOutput[0]);
        }
        {
            // For $regexMatch.
            auto expression = generateOptimizedExpression<ExpressionRegexMatch>(input, expCtx);
            auto regexMatchExpr = dynamic_cast<ExpressionRegexMatch*>(expression.get());
            ASSERT_EQ(regexMatchExpr->hasConstantRegex(), optimized);
            Value output = expression->evaluate({}, &expCtx->variables);
            ASSERT_VALUE_EQ(output, expectedFindAllOutput.empty() ? Value(false) : Value(true));
        }
    }
};

TEST(ExpressionRegexTest, BasicTest) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: 'asdf', regex: '^as' }}"),
        true,
        {Value(fromjson("{match: 'as', idx:0, captures:[]}"))});
}

TEST(ExpressionRegexTest, ExtendedRegexOptions) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: 'FirstLine\\nSecondLine', regex: "
                 "'^second' , options: 'mi'}}"),
        true,
        {Value(fromjson("{match: 'Second', idx:10, captures:[]}"))});
}

TEST(ExpressionRegexTest, MultipleMatches) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: 'a1b2c3', regex: '([a-c][1-3])' }}"),
        true,
        {Value(fromjson("{match: 'a1', idx:0, captures:['a1']}")),
         Value(fromjson("{match: 'b2', idx:2, captures:['b2']}")),
         Value(fromjson("{match: 'c3', idx:4, captures:['c3']}"))});
}

TEST(ExpressionRegexTest, OptimizPatternWhenInputIsVariable) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: '$input', regex: '([a-c][1-3])' }}"), true, {});
}

TEST(ExpressionRegexTest, NoOptimizePatternWhenRegexVariable) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: 'asdf', regex: '$regex' }}"), false, {});
}

TEST(ExpressionRegexTest, NoOptimizePatternWhenOptionsVariable) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: 'asdf', regex: '(asdf)', options: '$options' }}"),
        false,
        {Value(fromjson("{match: 'asdf', idx:0, captures:['asdf']}"))});
}

TEST(ExpressionRegexTest, NoMatch) {
    ExpressionRegexTest::testAllExpressions(
        fromjson("{$regexFindAll : {input: 'a1b2c3', regex: 'ab' }}"), true, {});
}

TEST(ExpressionRegexTest, FailureCaseBadRegexType) {
    ASSERT_THROWS_CODE(ExpressionRegexTest::testAllExpressions(
                           fromjson("{$regexFindAll : {input: 'FirstLine\\nSecondLine', regex: "
                                    "{invalid : 'regex'} , options: 'mi'}}"),
                           false,
                           {}),
                       AssertionException,
                       51105);
}

TEST(ExpressionRegexTest, FailureCaseBadRegexPattern) {
    ASSERT_THROWS_CODE(
        ExpressionRegexTest::testAllExpressions(
            fromjson("{$regexFindAll : {input: 'FirstLine\\nSecondLine', regex: '[0-9'}}"),
            false,
            {}),
        AssertionException,
        51111);
}

TEST(ExpressionRegexTest, InvalidUTF8InInput) {
    std::string inputField = "1234 ";
    // Append an invalid UTF-8 character.
    inputField += '\xe5';
    inputField += "  1234";
    BSONObj input(fromjson("{$regexFindAll: {input: '" + inputField + "', regex: '[0-9]'}}"));

    // Verify that PCRE will error during execution if input is not a valid UTF-8.
    ASSERT_THROWS_CODE(
        ExpressionRegexTest::testAllExpressions(input, true, {}), AssertionException, 51156);
}

TEST(ExpressionRegexTest, InvalidUTF8InRegex) {
    std::string regexField = "1234 ";
    // Append an invalid UTF-8 character.
    regexField += '\xe5';
    BSONObj input(fromjson("{$regexFindAll: {input: '123456', regex: '" + regexField + "'}}"));
    // Verify that PCRE will error if REGEX is not a valid UTF-8.
    ASSERT_THROWS_CODE(
        ExpressionRegexTest::testAllExpressions(input, false, {}), AssertionException, 51111);
}

}  // namespace ExpressionRegexTest

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() {
        add<Add::NullDocument>();
        add<Add::NoOperands>();
        add<Add::Date>();
        add<Add::String>();
        add<Add::Bool>();
        add<Add::Int>();
        add<Add::Long>();
        add<Add::Double>();
        add<Add::Null>();
        add<Add::Undefined>();
        add<Add::IntInt>();
        add<Add::IntIntNoOverflow>();
        add<Add::IntLong>();
        add<Add::IntLongOverflowToDouble>();
        add<Add::IntDouble>();
        add<Add::IntDate>();
        add<Add::LongDouble>();
        add<Add::LongDoubleNoOverflow>();
        add<Add::IntNull>();
        add<Add::LongUndefined>();

        add<CoerceToBool::EvaluateTrue>();
        add<CoerceToBool::EvaluateFalse>();
        add<CoerceToBool::Dependencies>();
        add<CoerceToBool::AddToBsonObj>();
        add<CoerceToBool::AddToBsonArray>();

        add<Constant::Create>();
        add<Constant::CreateFromBsonElement>();
        add<Constant::Optimize>();
        add<Constant::Dependencies>();
        add<Constant::AddToBsonObj>();
        add<Constant::AddToBsonArray>();

        add<Strcasecmp::NullBegin>();
        add<Strcasecmp::NullEnd>();
        add<Strcasecmp::NullMiddleLt>();
        add<Strcasecmp::NullMiddleEq>();
        add<Strcasecmp::NullMiddleGt>();

        add<SubstrBytes::FullNull>();
        add<SubstrBytes::BeginAtNull>();
        add<SubstrBytes::EndAtNull>();
        add<SubstrBytes::DropBeginningNull>();
        add<SubstrBytes::DropEndingNull>();
        add<SubstrBytes::NegativeLength>();

        add<ToLower::NullBegin>();
        add<ToLower::NullMiddle>();
        add<ToLower::NullEnd>();

        add<ToUpper::NullBegin>();
        add<ToUpper::NullMiddle>();
        add<ToUpper::NullEnd>();

        add<Set::Same>();
        add<Set::Redundant>();
        add<Set::DoubleRedundant>();
        add<Set::Sub>();
        add<Set::Super>();
        add<Set::SameBackwards>();
        add<Set::NoOverlap>();
        add<Set::Overlap>();
        add<Set::FirstNull>();
        add<Set::LeftNullAndRightEmpty>();
        add<Set::RightNullAndLeftEmpty>();
        add<Set::LastNull>();
        add<Set::NoArg>();
        add<Set::OneArg>();
        add<Set::EmptyArg>();
        add<Set::LeftArgEmpty>();
        add<Set::RightArgEmpty>();
        add<Set::ManyArgs>();
        add<Set::ManyArgsEqual>();

        add<AllAnyElements::JustFalse>();
        add<AllAnyElements::JustTrue>();
        add<AllAnyElements::OneTrueOneFalse>();
        add<AllAnyElements::Empty>();
        add<AllAnyElements::TrueViaInt>();
        add<AllAnyElements::FalseViaInt>();
        add<AllAnyElements::Null>();
    }
};

OldStyleSuiteInitializer<All> myAll;

namespace NowAndClusterTime {
TEST(NowAndClusterTime, BasicTest) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    // $$NOW is the Date type.
    {
        auto expression = ExpressionFieldPath::parse(expCtx, "$$NOW", expCtx->variablesParseState);
        Value result = expression->evaluate(Document(), &(expCtx->variables));
        ASSERT_EQ(result.getType(), Date);
    }
    // $$CLUSTER_TIME is the timestamp type.
    {
        auto expression =
            ExpressionFieldPath::parse(expCtx, "$$CLUSTER_TIME", expCtx->variablesParseState);
        Value result = expression->evaluate(Document(), &(expCtx->variables));
        ASSERT_EQ(result.getType(), bsonTimestamp);
    }

    // Multiple references to $$NOW must return the same value.
    {
        auto expression = Expression::parseExpression(
            expCtx, fromjson("{$eq: [\"$$NOW\", \"$$NOW\"]}"), expCtx->variablesParseState);
        Value result = expression->evaluate(Document(), &(expCtx->variables));

        ASSERT_VALUE_EQ(result, Value{true});
    }
    // Same is true for the $$CLUSTER_TIME.
    {
        auto expression =
            Expression::parseExpression(expCtx,
                                        fromjson("{$eq: [\"$$CLUSTER_TIME\", \"$$CLUSTER_TIME\"]}"),
                                        expCtx->variablesParseState);
        Value result = expression->evaluate(Document(), &(expCtx->variables));

        ASSERT_VALUE_EQ(result, Value{true});
    }
}
}  // namespace NowAndClusterTime
}  // namespace ExpressionTests

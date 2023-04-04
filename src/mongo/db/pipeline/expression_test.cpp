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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/summation.h"
#include "mongo/util/time_support.h"
#include <limits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace ExpressionTests {

using boost::intrusive_ptr;
using std::initializer_list;
using std::numeric_limits;
using std::pair;
using std::sort;
using std::string;
using std::vector;

/**
 * Creates an expression given by 'expressionName' and evaluates it using
 * 'operands' as inputs, returning the result.
 */
static Value evaluateExpression(const string& expressionName,
                                const vector<ImplicitValue>& operands) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON(expressionName << Value(ImplicitValue::convertToValues(operands)));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx.variables);
    return result;
}

/**
 * Takes the name of an expression as its first argument and a list of pairs of arguments and
 * expected results as its second argument, and asserts that for the given expression the arguments
 * evaluate to the expected results.
 */
static void assertExpectedResults(
    const string& expression,
    initializer_list<pair<initializer_list<ImplicitValue>, ImplicitValue>> operations) {
    for (auto&& op : operations) {
        try {
            Value result = evaluateExpression(expression, op.first);
            ASSERT_VALUE_EQ(op.second, result);
            ASSERT_EQUALS(op.second.getType(), result.getType());
        } catch (...) {
            LOGV2(24188, "failed", "argument"_attr = ImplicitValue::convertToValues(op.first));
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

/** Asserts that the Expression parsed from 'spec' returns a BSONArray and is equal to 'expected'.
 */
void assertExpectedArray(const BSONObj& spec, const BSONArray& expected) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = Expression::parseExpression(&expCtx, spec, vps);
    auto result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::Array);
    ASSERT_VALUE_EQ(result, Value(expected));
};

/**
 * Given 'parseFn', parses and evaluates 'spec' and verifies that the result is equal to
 * 'expected'. Useful when the parser for an expression is unavailable in certain contexts (for
 * instance, when evaluating an expression that's guarded by a feature flag that's off by default).
 */
void parseAndVerifyResults(
    const std::function<boost::intrusive_ptr<Expression>(
        ExpressionContext* const, BSONElement, const VariablesParseState&)>& parseFn,
    const BSONElement& elem,
    Value expected) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expr = parseFn(&expCtx, elem, vps);
    ASSERT_VALUE_EQ(expr->evaluate({}, &expCtx.variables), expected);
}

/**
 * A default redaction strategy that generates easy to check results for testing purposes.
 */
std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
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
                            Value(BSON("key1" << 2 << "key2" << 3))}});
}

TEST(ExpressionArrayToObjectTest, KVFormatWithDuplicates) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON("k"
                                                   << "hi"
                                                   << "v" << 2)
                                              << BSON("k"
                                                      << "hi"
                                                      << "v" << 3)))},
                            Value(BSON("hi" << 3))}});
}

TEST(ExpressionArrayToObjectTest, ListFormatSimple) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON_ARRAY("key1" << 2) << BSON_ARRAY("key2" << 3)))},
                            Value(BSON("key1" << 2 << "key2" << 3))}});
}

TEST(ExpressionArrayToObjectTest, ListFormWithDuplicates) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON_ARRAY("key1" << 2) << BSON_ARRAY("key1" << 3)))},
                            Value(BSON("key1" << 3))}});
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

/* ------------------------ ExpressionSortArray -------------------- */

TEST(ExpressionSortArrayTest, SortsNormalArrayForwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ 2, 1, 3 ] }, sortBy: 1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::Array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}


TEST(ExpressionSortArrayTest, SortsNormalArrayBackwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ 2, 1, 3 ] }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::Array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(3 << 2 << 1)));
}

TEST(ExpressionSortArrayTest, SortsEmptyArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ ] }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::Array);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>()));
}

TEST(ExpressionSortArrayTest, SortsOneElementArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ 1 ] }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::Array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1)));
}

TEST(ExpressionSortArrayTest, ReturnsNullWithNullInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: null }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionSortArrayTest, ReturnsNullWithUndefinedInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: undefined }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

/* ------------------------- Old-style tests -------------------------- */

namespace Add {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
        populateOperands(expression);
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(expression->evaluate({}, &expCtx.variables)));
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
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
        expression->addOperand(ExpressionConstant::create(&expCtx, Value(2)));
        ASSERT_BSONOBJ_EQ(BSON("" << 2), toBson(expression->evaluate({}, &expCtx.variables)));
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
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
        expression->addOperand(ExpressionConstant::create(&expCtx, Value("a"_sd)));
        ASSERT_THROWS(expression->evaluate({}, &expCtx.variables), AssertionException);
    }
};

/** Bool type unsupported. */
class Bool {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(&expCtx);
        expression->addOperand(ExpressionConstant::create(&expCtx, Value(true)));
        ASSERT_THROWS(expression->evaluate({}, &expCtx.variables), AssertionException);
    }
};

class SingleOperandBase : public ExpectedResultBase {
    void populateOperands(intrusive_ptr<ExpressionNary>& expression) {
        expression->addOperand(ExpressionConstant::create(&expCtx, valueFromBson(operand())));
    }
    BSONObj expectedResult() {
        return operand();
    }

protected:
    ExpressionContextForTest expCtx;
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
        auto expCtx = ExpressionContextForTest{};
        expression->addOperand(
            ExpressionConstant::create(&expCtx, valueFromBson(_reverse ? operand2() : operand1())));
        expression->addOperand(
            ExpressionConstant::create(&expCtx, valueFromBson(_reverse ? operand1() : operand2())));
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
        return BSON("" << static_cast<double>(numeric_limits<long long>::max()) +
                        static_cast<double>(numeric_limits<long long>::max()));
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
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> nested = ExpressionConstant::create(&expCtx, Value(5));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(&expCtx, nested);
        ASSERT(expression->evaluate({}, &expCtx.variables).getBool());
    }
};

/** Nested expression coerced to false. */
class EvaluateFalse {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> nested = ExpressionConstant::create(&expCtx, Value(0));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(&expCtx, nested);
        ASSERT(!expression->evaluate({}, &expCtx.variables).getBool());
    }
};

/** Dependencies forwarded from nested expression. */
class Dependencies {
public:
    void run() {
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> nested = ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b");
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(&expCtx, nested);
        DepsTracker dependencies;
        expression::addDependencies(expression.get(), &dependencies);
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
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(
            &expCtx, ExpressionFieldPath::deprecatedCreate(&expCtx, "foo"));

        // serialized as $and because CoerceToBool isn't an ExpressionNary
        ASSERT_BSONOBJ_BINARY_EQ(fromjson("{field:{$and:['$foo']}}"), toBsonObj(expression));
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
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(
            &expCtx, ExpressionFieldPath::deprecatedCreate(&expCtx, "foo"));

        // serialized as $and because CoerceToBool isn't an ExpressionNary
        ASSERT_BSONOBJ_BINARY_EQ(BSON_ARRAY(fromjson("{$and:['$foo']}")), toBsonArray(expression));
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
        auto expCtx = ExpressionContextForTest{};
        intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
        ASSERT_BSONOBJ_BINARY_EQ(BSON("" << 5),
                                 toBson(expression->evaluate({}, &expCtx.variables)));
    }
};

/** Create an ExpressionConstant from a BsonElement. */
class CreateFromBsonElement {
public:
    void run() {
        BSONObj spec = BSON("IGNORED_FIELD_NAME"
                            << "foo");
        auto expCtx = ExpressionContextForTest{};
        BSONElement specElement = spec.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = ExpressionConstant::parse(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_BINARY_EQ(BSON(""
                                      << "foo"),
                                 toBson(expression->evaluate({}, &expCtx.variables)));
    }
};

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
        return BSON("field" << expression->serialize(false));
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
        bab << expression->serialize(false);
        return bab.obj();
    }
};

TEST(ExpressionConstantTest, ConstantOfValueMissingRemovesField) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value());
    ASSERT_BSONOBJ_BINARY_EQ(
        BSONObj(),
        toBson(expression->evaluate(Document{{"foo", Value("bar"_sd)}}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, ConstantOfValueMissingSerializesToRemoveSystemVar) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value());
    ASSERT_BSONOBJ_BINARY_EQ(BSON("field"
                                  << "$$REMOVE"),
                             BSON("field" << expression->serialize(false)));
}

TEST(ExpressionConstantTest, ConstantRedaction) {
    SerializationOptions options;
    std::string replacementChar = "?";
    options.replacementForLiteralArgs = replacementChar;

    // Test that a constant is replaced.
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value("my_ssn"_sd));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":{"$const":"?"}})",
        BSON("field" << expression->serialize(options)));

    auto expressionBSON = BSON("$and" << BSON_ARRAY(BSON("$gt" << BSON_ARRAY("$foo" << 5))
                                                    << BSON("$lt" << BSON_ARRAY("$foo" << 10))));
    expression = Expression::parseExpression(&expCtx, expressionBSON, expCtx.variablesParseState);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$and": [
                    {
                        "$gt": [
                            "$foo",
                            {
                                "$const": "?"
                            }
                        ]
                    },
                    {
                        "$lt": [
                            "$foo",
                            {
                                "$const": "?"
                            }
                        ]
                    }
                ]
            }
        })",
        BSON("field" << expression->serialize(options)));
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

TEST(ExpressionFromAccumulators, FirstNLastN) {
    using Sense = AccumulatorFirstLastN::Sense;

    // $firstN
    auto firstNFn = [&](const BSONObj& spec, const BSONArray& expected) {
        parseAndVerifyResults(AccumulatorFirstLastN::parseExpression<Sense::kFirst>,
                              spec.firstElement(),
                              Value(expected));
    };
    firstNFn(fromjson("{$firstN: {n: 3, input: [19, 7, 28, 3, 5]}}"),
             BSONArray(fromjson("[19, 7, 28]")));
    firstNFn(fromjson("{$firstN: {n: 6, input: [19, 7, 28, 3, 5]}}"),
             BSONArray(fromjson("[19, 7, 28, 3, 5]")));
    firstNFn(fromjson("{$firstN: {n: 3, input: [1,2,3,4,5,6]}}"), BSONArray(fromjson("[1,2,3]")));
    firstNFn(fromjson("{$firstN: {n: 3, input: [1,2,null,null]}}"),
             BSONArray(fromjson("[1,2,null]")));
    firstNFn(fromjson("{$firstN: {n: 3, input: [1.1, 2.713, 3, 3.4]}}"),
             BSONArray(fromjson("[1.1, 2.713, 3]")));

    // $lastN
    auto lastNFn = [&](const BSONObj& spec, const BSONArray& expected) {
        parseAndVerifyResults(AccumulatorFirstLastN::parseExpression<Sense::kLast>,
                              spec.firstElement(),
                              Value(expected));
    };
    lastNFn(fromjson("{$lastN: {n: 3, input: [19, 7, 28, 3, 5]}}"),
            BSONArray(fromjson("[28,3,5]")));
    lastNFn(fromjson("{$lastN: {n: 6, input: [19, 7, 28, 3, 5]}}"),
            BSONArray(fromjson("[19, 7, 28, 3, 5]")));
    lastNFn(fromjson("{$lastN: {n: 3, input: [3,2,1,4,5,6]}}"), BSONArray(fromjson("[4,5,6]")));
    lastNFn(fromjson("{$lastN: {n: 3, input: [1,2,null,3]}}"), BSONArray(fromjson("[2,null,3]")));
    lastNFn(fromjson("{$lastN: {n: 3, input: [3, 2.713, 1.1, 2.7]}}"),
            BSONArray(fromjson("[2.713, 1.1, 2.7]")));
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

TEST(ExpressionFromAccumulators, MinNMaxN) {
    using Sense = AccumulatorMinMax::Sense;
    auto maxNFn = [&](const BSONObj& spec, const BSONArray& expected) {
        parseAndVerifyResults(
            AccumulatorMinMaxN::parseExpression<Sense::kMax>, spec.firstElement(), Value(expected));
    };

    // $maxN
    maxNFn(fromjson("{$maxN: {n: 3, input: [19, 7, 28, 3, 5]}}"),
           BSONArray(fromjson("[28, 19, 7]")));
    maxNFn(fromjson("{$maxN: {n: 6, input: [19, 7, 28, 3, 5]}}"),
           BSONArray(fromjson("[28, 19, 7, 5, 3]")));
    maxNFn(fromjson("{$maxN: {n: 3, input: [1,2,3]}}"), BSONArray(fromjson("[3,2,1]")));
    maxNFn(fromjson("{$maxN: {n: 3, input: [1,2,null]}}"), BSONArray(fromjson("[2,1]")));
    maxNFn(fromjson("{$maxN: {n: 3, input: [1.1, 2.713, 3]}}"),
           BSONArray(fromjson("[3, 2.713, 1.1]")));

    auto minNFn = [&](const BSONObj& spec, const BSONArray& expected) {
        parseAndVerifyResults(
            AccumulatorMinMaxN::parseExpression<Sense::kMin>, spec.firstElement(), Value(expected));
    };

    // $minN
    minNFn(fromjson("{$minN: {n: 3, input: [19, 7, 28, 3, 5]}}"), BSONArray(fromjson("[3,5,7]")));
    minNFn(fromjson("{$minN: {n: 6, input: [19, 7, 28, 3, 5]}}"),
           BSONArray(fromjson("[3,5,7,19, 28]")));
    minNFn(fromjson("{$minN: {n: 3, input: [3,2,1]}}"), BSONArray(fromjson("[1,2,3]")));
    minNFn(fromjson("{$minN: {n: 3, input: [1,2,null]}}"), BSONArray(fromjson("[1,2]")));
    minNFn(fromjson("{$minN: {n: 3, input: [3, 2.713, 1.1]}}"),
           BSONArray(fromjson("[1.1, 2.713, 3]")));
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

TEST(ExpressionFromAccumulators, StdDevSampRepeated) {
    ExpressionContextForTest expCtx;
    AccumulatorStdDevPop mergedAcc(&expCtx);

    for (int i = 0; i < 100; i++) {
        AccumulatorStdDevPop acc(&expCtx);
        acc.process(Value(std::exp(14.0)), false /*merging*/);
        Value mergedValue = acc.getValue(true /*toBeMerged*/);
        mergedAcc.process(mergedValue, true /*merging*/);
    }

    Value result = mergedAcc.getValue(false /*toBeMerged*/);
    const double doubleVal = result.coerceToDouble();
    ASSERT_EQ(0.0, doubleVal);
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
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto expr =
        Expression::parseExpression(&expCtx, BSON("$pow" << BSON_ARRAY(0 << -5)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);

    const auto exprWithLong =
        Expression::parseExpression(&expCtx, BSON("$pow" << BSON_ARRAY(0LL << -5LL)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
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
    auto expIndexInRangeWithhDuplicateValues = Expression::parseExpression(
        &expCtx,
        // Search for 2 between 4 and 6.
        fromjson("{ $indexOfArray : [ [0, 1, 2, 2, 2, 2, 4, 5] , '$x', 4, 6] }"),
        expCtx.variablesParseState);
    auto optimizedIndexInRangeWithDuplcateValues = expIndexInRangeWithhDuplicateValues->optimize();
    // Should evaluate to 4.
    ASSERT_VALUE_EQ(
        Value(4),
        optimizedIndexInRangeWithDuplcateValues->evaluate(Document{{"x", 2}}, &expCtx.variables));
}

TEST(ExpressionInternalFindAllValuesAtPath, PreservesSimpleArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    auto result =
        expression->evaluate(Document{{"a", Value({Value(1), Value(2)})}}, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, PreservesSimpleNestedArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    auto doc = Document{{"a", Value(Document{{"b", Value({Value(1), Value(2)})}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DescendsThroughSingleArrayAndObject) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document doc = Document{
        {"a",
         Value({Document{{"b", Value(1)}}, Document{{"b", Value(2)}}, Document{{"b", Value(3)}}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DescendsThroughMultipleObjectArrayPairs) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document doc = Document{{"a",
                             Value({Document{{"b", Value({Value(1), Value(2)})}},
                                    Document{{"b", Value({Value(3), Value(4)})}},
                                    Document{{"b", Value({Value(5), Value(6)})}}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DoesNotDescendThroughDoubleArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document seenDoc1 = Document{{"b", Value({Value(5), Value(6)})}};
    Document seenDoc2 = Document{{"b", Value({Value(3), Value(4)})}};
    Document unseenDoc1 = Document{{"b", Value({Value(1), Value(2)})}};
    Document unseenDoc2 = Document{{"b", Value({Value(7), Value(8)})}};

    Document doc = Document{{"a",
                             Value({
                                 Value({unseenDoc1, unseenDoc2}),
                                 Value(seenDoc1),
                                 Value(seenDoc2),
                             })}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(3 << 4 << 5 << 6)), result);
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
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    return Expression::parseExpression(&expCtx, specification, vps);
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
    auto expCtx = ExpressionContextForTest{};
    BSONElement specElement = specification.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    return Expression::parseOperand(&expCtx, specElement, vps);
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
        auto expCtx = ExpressionContextForTest{};
        const Document spec = getSpec();
        const Value args = spec["input"];
        if (!spec["expected"].missing()) {
            FieldIterator fields(spec["expected"].getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value expected = field.second;
                const BSONObj obj = BSON(field.first << args);
                VariablesParseState vps = expCtx.variablesParseState;
                const intrusive_ptr<Expression> expr =
                    Expression::parseExpression(&expCtx, obj, vps);
                Value result = expr->evaluate({}, &expCtx.variables);
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
                VariablesParseState vps = expCtx.variablesParseState;
                ASSERT_THROWS(
                    [&] {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(&expCtx, obj, vps);
                        expr->evaluate({}, &expCtx.variables);
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
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObj = BSON("" << spec);
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                          toBson(expression->evaluate({}, &expCtx.variables)));
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
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate({}, &expCtx.variables)));
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
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto str = "abcdef"_sd;
    const auto expr =
        Expression::parseExpression(&expCtx, BSON("$substrCP" << BSON_ARRAY(str << -5 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

}  // namespace SubstrBytes

namespace SubstrCP {

TEST(ExpressionSubstrCPTest, DoesThrowWithBadContinuationByte) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto continuationByte = "\x80\x00"_sd;
    const auto expr = Expression::parseExpression(
        &expCtx, BSON("$substrCP" << BSON_ARRAY(continuationByte << 0 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionSubstrCPTest, DoesThrowWithInvalidLeadingByte) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto leadingByte = "\xFF\x00"_sd;
    const auto expr = Expression::parseExpression(
        &expCtx, BSON("$substrCP" << BSON_ARRAY(leadingByte << 0 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
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
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = ExpressionFieldPath::parse(&expCtx, "$$REMOVE", vps);
    ASSERT_BSONOBJ_EQ(BSON("foo"
                           << "$$REMOVE"),
                      BSON("foo" << expression->serialize(false)));
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectlyWithTrailingPath) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = ExpressionFieldPath::parse(&expCtx, "$$REMOVE.a.b", vps);
    ASSERT_BSONOBJ_EQ(BSON("foo"
                           << "$$REMOVE.a.b"),
                      BSON("foo" << expression->serialize(false)));
}

TEST(BuiltinRemoveVariableTest, RemoveSerializesCorrectlyAfterOptimization) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    auto expression = ExpressionFieldPath::parse(&expCtx, "$$REMOVE.a.b", vps);
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
    assertExpectedResults("$mergeObjects", {{{}, Document({})}});

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
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate({}, &expCtx.variables)));
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
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObj = BSON("" << spec());
        BSONElement specElement = specObj.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate({}, &expCtx.variables)));
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
        auto expCtx = ExpressionContextForTest{};
        const Document spec = getSpec();
        const Value args = spec["input"];
        if (!spec["expected"].missing()) {
            FieldIterator fields(spec["expected"].getDocument());
            while (fields.more()) {
                const Document::FieldPair field(fields.next());
                const Value expected = field.second;
                const BSONObj obj = BSON(field.first << args);
                VariablesParseState vps = expCtx.variablesParseState;
                const intrusive_ptr<Expression> expr =
                    Expression::parseExpression(&expCtx, obj, vps);
                const Value result = expr->evaluate({}, &expCtx.variables);
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
                VariablesParseState vps = expCtx.variablesParseState;
                ASSERT_THROWS(
                    [&] {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(&expCtx, obj, vps);
                        expr->evaluate({}, &expCtx.variables);
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
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 2u);
    ASSERT_EQ(computedPaths.renames["h.a.b"], "c");
    ASSERT_EQ(computedPaths.renames["h.d.f"], "e.g");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPaths) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject =
        fromjson("{$map: {input: '$a', as: 'iter', in: {b: '$$iter.c', d: {$add: [1, 2]}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e.d"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["e.b"], "a.c");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPathsWithDefaultVarName) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a', in: {b: '$$this.c', d: {$add: [1, 2]}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e.d"), 1u);
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["e.b"], "a.c");
}

TEST(GetComputedPathsTest, ExpressionMapCorrectlyReportsComputedPathsWithNestedExprObject) {
    auto expCtx = ExpressionContextForTest{};
    auto specObject = fromjson("{$map: {input: '$a', in: {b: {c: '$$this.d'}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT(computedPaths.paths.empty());
    ASSERT_EQ(computedPaths.renames.size(), 1u);
    ASSERT_EQ(computedPaths.renames["e.b.c"], "a.d");
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
    auto specObject = fromjson("{$map: {input: '$a.b', as: 'iter', in: {c: '$$iter.d'}}}}");
    auto expr = Expression::parseObject(&expCtx, specObject, expCtx.variablesParseState);
    ASSERT(dynamic_cast<ExpressionMap*>(expr.get()));
    auto computedPaths = expr->getComputedPaths("e");
    ASSERT_EQ(computedPaths.paths.size(), 1u);
    ASSERT_EQ(computedPaths.paths.count("e"), 1u);
    ASSERT(computedPaths.renames.empty());
}

}  // namespace GetComputedPathsTest

namespace expression_meta_test {
TEST(ExpressionMetaTest, ExpressionMetaSearchScore) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchScore\"}");
    auto expressionMeta = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);
    MutableDocument doc;
    doc.metadata().setSearchScore(1.234);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.234);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchScoreAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.opCtx).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchScore\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchHighlights) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchHighlights\"}");
    auto expressionMeta = ExpressionMeta::parse(&expCtx, expr.firstElement(), vps);

    MutableDocument doc;
    Document highlights = DOC("this part" << 1 << "is opaque to the server" << 1);
    doc.metadata().setSearchHighlights(Value(highlights));

    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), highlights);
}

TEST(ExpressionMetaTest, ExpressionMetasearchHighlightsAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.opCtx).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"searchHighlights\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaGeoNearDistance) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"geoNearDistance\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setGeoNearDistance(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaGeoNearPoint) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"geoNearPoint\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    Document pointDoc = Document{fromjson("{some: 'document'}")};
    doc.metadata().setGeoNearPoint(Value(pointDoc));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), pointDoc);
}

TEST(ExpressionMetaTest, ExpressionMetaIndexKey) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"indexKey\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    BSONObj ixKey = fromjson("{'': 1, '': 'string'}");
    doc.metadata().setIndexKey(ixKey);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), Document(ixKey));
}

TEST(ExpressionMetaTest, ExpressionMetaIndexKeyAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.opCtx).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"indexKey\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaRecordId) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"recordId\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setRecordId(RecordId(123LL));
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getLong(), 123LL);
}

TEST(ExpressionMetaTest, ExpressionMetaRandVal) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"randVal\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setRandVal(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaSortKey) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"sortKey\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    Value sortKey = Value(std::vector<Value>{Value(1), Value(2)});
    doc.metadata().setSortKey(sortKey, /* isSingleElementSortKey = */ false);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>{Value(1), Value(2)}));
}

TEST(ExpressionMetaTest, ExpressionMetaTextScore) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"textScore\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    MutableDocument doc;
    doc.metadata().setTextScore(1.23);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_EQ(val.getDouble(), 1.23);
}

TEST(ExpressionMetaTest, ExpressionMetaTextScoreAPIStrict) {
    auto expCtx = ExpressionContextForTest{};
    APIParameters::get(expCtx.opCtx).setAPIStrict(true);
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"textScore\"}");
    ASSERT_THROWS_CODE(ExpressionMeta::parse(&expCtx, expr.firstElement(), vps),
                       AssertionException,
                       ErrorCodes::APIStrictError);
}

TEST(ExpressionMetaTest, ExpressionMetaSearchScoreDetails) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{$meta: \"searchScoreDetails\"}");
    auto expressionMeta =
        ExpressionMeta::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    auto details = BSON("scoreDetails"
                        << "foo");
    MutableDocument doc;
    doc.metadata().setSearchScoreDetails(details);
    Value val = expressionMeta->evaluate(doc.freeze(), &expCtx.variables);
    ASSERT_DOCUMENT_EQ(val.getDocument(), Document(details));
}
}  // namespace expression_meta_test

namespace ExpressionRegexTest {

class ExpressionRegexTest {
public:
    template <typename ExpressionRegexSubClass>
    static intrusive_ptr<Expression> generateOptimizedExpression(const BSONObj& input,
                                                                 ExpressionContextForTest* expCtx) {

        auto expression = ExpressionRegexSubClass::parse(
            expCtx, input.firstElement(), expCtx->variablesParseState);
        return expression->optimize();
    }

    static void testAllExpressions(const BSONObj& input,
                                   bool optimized,
                                   const std::vector<Value>& expectedFindAllOutput) {

        auto expCtx = ExpressionContextForTest{};
        {
            // For $regexFindAll.
            auto expression = generateOptimizedExpression<ExpressionRegexFindAll>(input, &expCtx);
            auto regexFindAllExpr = dynamic_cast<ExpressionRegexFindAll*>(expression.get());
            ASSERT_EQ(regexFindAllExpr->hasConstantRegex(), optimized);
            Value output = expression->evaluate({}, &expCtx.variables);
            ASSERT_VALUE_EQ(output, Value(expectedFindAllOutput));
        }
        {
            // For $regexFind.
            auto expression = generateOptimizedExpression<ExpressionRegexFind>(input, &expCtx);
            auto regexFindExpr = dynamic_cast<ExpressionRegexFind*>(expression.get());
            ASSERT_EQ(regexFindExpr->hasConstantRegex(), optimized);
            Value output = expression->evaluate({}, &expCtx.variables);
            ASSERT_VALUE_EQ(
                output, expectedFindAllOutput.empty() ? Value(BSONNULL) : expectedFindAllOutput[0]);
        }
        {
            // For $regexMatch.
            auto expression = generateOptimizedExpression<ExpressionRegexMatch>(input, &expCtx);
            auto regexMatchExpr = dynamic_cast<ExpressionRegexMatch*>(expression.get());
            ASSERT_EQ(regexMatchExpr->hasConstantRegex(), optimized);
            Value output = expression->evaluate({}, &expCtx.variables);
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

class All : public unittest::OldStyleSuiteSpecification {
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

unittest::OldStyleSuiteInitializer<All> myAll;

namespace NowAndClusterTime {
TEST(NowAndClusterTime, BasicTest) {
    auto expCtx = ExpressionContextForTest{};

    // $$NOW is the Date type.
    {
        auto expression = ExpressionFieldPath::parse(&expCtx, "$$NOW", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);
        ASSERT_EQ(result.getType(), Date);
    }
    // $$CLUSTER_TIME is the timestamp type.
    {
        auto expression =
            ExpressionFieldPath::parse(&expCtx, "$$CLUSTER_TIME", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);
        ASSERT_EQ(result.getType(), bsonTimestamp);
    }

    // Multiple references to $$NOW must return the same value.
    {
        auto expression = Expression::parseExpression(
            &expCtx, fromjson("{$eq: [\"$$NOW\", \"$$NOW\"]}"), expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);

        ASSERT_VALUE_EQ(result, Value{true});
    }
    // Same is true for the $$CLUSTER_TIME.
    {
        auto expression =
            Expression::parseExpression(&expCtx,
                                        fromjson("{$eq: [\"$$CLUSTER_TIME\", \"$$CLUSTER_TIME\"]}"),
                                        expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables);

        ASSERT_VALUE_EQ(result, Value{true});
    }
}
}  // namespace NowAndClusterTime

void assertRandomProperties(const std::function<double(void)>& fn) {
    double sum = 0.0;
    constexpr int N = 1000000;

    for (int i = 0; i < N; i++) {
        const double v = fn();
        ASSERT_LTE(0.0, v);
        ASSERT_GTE(1.0, v);
        sum += v;
    }

    const double avg = sum / N;
    // For continuous uniform distribution [0.0, 1.0] the variance is 1/12.
    // Test certainty within 10 standard deviations.
    const double err = 10.0 / sqrt(12.0 * N);
    ASSERT_LT(0.5 - err, avg);
    ASSERT_GT(0.5 + err, avg);
}

TEST(ExpressionRandom, Basic) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // We generate a new random value on every call to evaluate().
    intrusive_ptr<Expression> expression =
        Expression::parseExpression(&expCtx, fromjson("{ $rand: {} }"), vps);

    const std::string& serialized = expression->serialize(false).getDocument().toString();
    ASSERT_EQ("{$rand: {}}", serialized);

    const auto randFn = [&expression, &expCtx]() -> double {
        return expression->evaluate({}, &expCtx.variables).getDouble();
    };
    assertRandomProperties(randFn);
}

namespace ExpressionToHashedIndexKeyTest {

TEST(ExpressionToHashedIndexKeyTest, StringInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey"
                             << "hashThisStringLiteral"_sd);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-5776344739422278694));
}

TEST(ExpressionToHashedIndexKeyTest, IntInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << 123);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-6548868637522515075));
}

TEST(ExpressionToHashedIndexKeyTest, TimestampInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << Timestamp(0, 0));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-7867208682377458672));
}

TEST(ExpressionToHashedIndexKeyTest, ObjectIdInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << OID("47cc67093475061e3d95369d"));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(1576265281381834298));
}

TEST(ExpressionToHashedIndexKeyTest, DateInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << Date_t());
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-1178696894582842035));
}

TEST(ExpressionToHashedIndexKeyTest, MissingInputValueSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey"
                             << "$missingField");
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2338878944348059895));
}

TEST(ExpressionToHashedIndexKeyTest, NullInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSONNULL);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2338878944348059895));
}

TEST(ExpressionToHashedIndexKeyTest, ExpressionInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSON("$pow" << BSON_ARRAY(2 << 4)));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2598032665634823220));
}

TEST(ExpressionToHashedIndexKeyTest, UndefinedInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSONUndefined);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(40158834000849533LL));
}

TEST(ExpressionToHashedIndexKeyTest, DoesAddInputDependencies) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey"
                             << "$someValue");
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);

    DepsTracker deps;
    expression::addDependencies(expression.get(), &deps);
    ASSERT_EQ(deps.fields.count("someValue"), 1u);
    ASSERT_EQ(deps.fields.size(), 1u);
}
}  // namespace ExpressionToHashedIndexKeyTest

TEST(ExpressionSubtractTest, OverflowLong) {
    const auto maxLong = std::numeric_limits<long long int>::max();
    const auto minLong = std::numeric_limits<long long int>::min();
    auto expCtx = ExpressionContextForTest{};

    // The following subtractions should not fit into a long long data type.
    BSONObj obj = BSON("$subtract" << BSON_ARRAY(maxLong << minLong));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_EQ(result.getDouble(), static_cast<double>(maxLong) - minLong);

    obj = BSON("$subtract" << BSON_ARRAY(minLong << maxLong));
    expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_EQ(result.getDouble(), static_cast<double>(minLong) - static_cast<double>(maxLong));

    // minLong = -1 - maxLong. The below subtraction should fit into long long data type.
    obj = BSON("$subtract" << BSON_ARRAY(-1 << maxLong));
    expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::NumberLong);
    ASSERT_EQ(result.getLong(), -1LL - maxLong);

    // The minLong's negation does not fit into long long, hence it should be converted to double
    // data type.
    obj = BSON("$subtract" << BSON_ARRAY(0 << minLong));
    expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    result = expression->evaluate({}, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::NumberDouble);
    ASSERT_EQ(result.getDouble(), static_cast<double>(minLong) * -1);
}

TEST(ExpressionGetFieldTest, GetFieldSerializesStringArgumentCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: \"foo\"}");
    auto expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_BSONOBJ_EQ(BSON("ignoredField" << BSON("$getField" << BSON("field" << BSON("$const"
                                                                                      << "foo")
                                                                              << "input"
                                                                              << "$$CURRENT"))),
                      BSON("ignoredField" << expression->serialize(false)));
}

TEST(ExpressionGetFieldTest, GetFieldSerializesCorrectly) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    BSONObj expr = fromjson("{$meta: {\"field\": \"foo\", \"input\": {a: 1}}}");
    auto expression = ExpressionGetField::parse(&expCtx, expr.firstElement(), vps);
    ASSERT_BSONOBJ_EQ(
        BSON("ignoredField" << BSON(
                 "$getField" << BSON("field" << BSON("$const"
                                                     << "foo")
                                             << "input" << BSON("a" << BSON("$const" << 1))))),
        BSON("ignoredField" << expression->serialize(false)));
}

TEST(ExpressionGetFieldTest, GetFieldSerializesAndRedactsCorrectly) {
    SerializationOptions options;
    std::string replacementChar = "?";
    options.replacementForLiteralArgs = replacementChar;
    options.identifierRedactionPolicy = redactFieldNameForTest;
    options.redactIdentifiers = true;
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    BSONObj expressionBSON = BSON("$getField" << BSON("field"
                                                      << "a"
                                                      << "input"
                                                      << "$b"));

    auto expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":{"$getField":{"field":"HASH<a>","input":"$HASH<b>"}}})",
        BSON("field" << expression->serialize(options)));

    // Test the shorthand syntax.
    expressionBSON = BSON("$getField"
                          << "a");

    expression = ExpressionGetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"field":{"$getField":{"field":"HASH<a>","input":"$$CURRENT"}}})",
        BSON("field" << expression->serialize(options)));

    // Test a field with '.' characters.
    expressionBSON = BSON("$getField"
                          << "a.b.c");

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
}

TEST(ExpressionSetFieldTest, SetFieldRedactsCorrectly) {
    SerializationOptions options;
    std::string replacementChar = "?";
    options.replacementForLiteralArgs = replacementChar;
    options.identifierRedactionPolicy = redactFieldNameForTest;
    options.redactIdentifiers = true;
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    // Test that a set field redacts properly.
    BSONObj expressionBSON = BSON("$setField" << BSON("field"
                                                      << "a"
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
    expressionBSON = BSON("$setField" << BSON("field"
                                              << "a"
                                              << "input" << BSON("a" << true) << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": {
                        "$const": "?"
                    },
                    "value": {
                        "$const": "?"
                    }
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Nested object as input.
    expressionBSON =
        BSON("$setField" << BSON("field"
                                 << "a"
                                 << "input" << BSON("a" << BSON("b" << 5)) << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": {
                        "$const": "?"
                    },
                    "value": {
                        "$const": "?"
                    }
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Object with field path in input.
    expressionBSON = BSON("$setField" << BSON("field"
                                              << "a"
                                              << "input"
                                              << BSON("a"
                                                      << "$field")
                                              << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": {
                        "HASH<a>": "$HASH<field>"
                    },
                    "value": {
                        "$const": "?"
                    }
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Object with field path in value.
    expressionBSON = BSON("$setField" << BSON("field"
                                              << "a"
                                              << "input"
                                              << BSON("a"
                                                      << "b")
                                              << "value"
                                              << BSON("c"
                                                      << "$d")));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": {
                        "$const": "?"
                    },
                    "value": {
                        "HASH<c>": "$HASH<d>"
                    }
                }
            }
        })",
        BSON("field" << expression->serialize(options)));

    // Array as input.
    expressionBSON = BSON("$setField" << BSON("field"
                                              << "a"
                                              << "input" << BSON("a" << BSON_ARRAY(3 << 4 << 5))
                                              << "value" << 10));
    expression = ExpressionSetField::parse(&expCtx, expressionBSON.firstElement(), vps);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "field": {
                "$setField": {
                    "field": "HASH<a>",
                    "input": {
                        "$const": "?"
                    },
                    "value": {
                        "$const": "?"
                    }
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
    ASSERT_BSONOBJ_EQ(
        BSON("ignoredField" << BSON("$setField"
                                    << BSON("field" << BSON("$const"
                                                            << "foo")
                                                    << "input" << BSON("a" << BSON("$const" << 1))
                                                    << "value" << BSON("$const" << 24)))),
        BSON("ignoredField" << expression->serialize(false)));
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
    ASSERT_VALUE_EQ(optimizedNullRemoved->serialize(false), Value("$a"_sd));
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

TEST(ExpressionAddTest, Integers) {
    assertExpectedResults("$add",
                          {
                              // Empty case.
                              {{}, 0},
                              // Singleton case.
                              {{1}, 1},
                              // Integer addition.
                              {{1, 2, 3}, 6},
                              // Adding negative numbers
                              {{6, -3, 2}, 5},
                              // Getting a negative result
                              {{-6, -3, 2}, -7},
                              // Min/max ints are not promoted to longs.
                              {{INT_MAX}, INT_MAX},
                              {{INT_MAX, -1}, Value(INT_MAX - 1)},
                              {{INT_MIN}, INT_MIN},
                              {{INT_MIN, 1}, Value(INT_MIN + 1)},
                              // Integer overflow is promoted to a long.
                              {{INT_MAX, 1}, Value((long long)INT_MAX + 1LL)},
                              {{INT_MIN, -1}, Value((long long)INT_MIN - 1LL)},
                          });
}


TEST(ExpressionAddTest, Longs) {
    assertExpectedResults(
        "$add",
        {
            // Singleton case.
            {{1LL}, 1LL},
            // Long addition.
            {{1LL, 2LL, 3LL}, 6LL},
            // Adding negative numbers
            {{6LL, -3LL, 2LL}, 5LL},
            // Getting a negative result
            {{-6LL, -3LL, 2LL}, -7LL},
            // Confirm that NumberLong is wider than NumberInt, and the output
            // will be a long if any operand is a long.
            {{1LL, 2, 3LL}, 6LL},
            {{1LL, 2, 3}, 6LL},
            {{1, 2, 3LL}, 6LL},
            {{1, 2LL, 3LL}, 6LL},
            {{6, -3LL, 2}, 5LL},
            {{-6LL, -3, 2}, -7LL},
            // Min/max longs are not promoted to double.
            {{LLONG_MAX}, LLONG_MAX},
            {{LLONG_MAX, -1LL}, Value(LLONG_MAX - 1LL)},
            {{LLONG_MIN}, LLONG_MIN},
            {{LLONG_MIN, 1LL}, Value(LLONG_MIN + 1LL)},
            // Long overflow is promoted to a double.
            {{LLONG_MAX, 1LL}, Value((double)LLONG_MAX + 1.0)},
            // The result is "incorrect" here due to floating-point rounding errors.
            {{LLONG_MIN, -1LL}, Value((double)LLONG_MIN)},
        });
}

TEST(ExpressionAddTest, Doubles) {
    assertExpectedResults("$add",
                          {
                              // Singleton case.
                              {{1.0}, 1.0},
                              // Double addition.
                              {{1.0, 2.0, 3.0}, 6.0},
                              // Adding negative numbers
                              {{6.0, -3.0, 2.0}, 5.0},
                              // Getting a negative result
                              {{-6.0, -3.0, 2.0}, -7.0},
                              // Confirm that doubles are wider than ints and longs, and the output
                              // will be a double if any operand is a double.
                              {{1, 2, 3.0}, 6.0},
                              {{1LL, 2LL, 3.0}, 6.0},
                              {{3.0, 2, 1LL}, 6.0},
                              {{3, 2.0, 1LL}, 6.0},
                              {{-3, 2.0, 1LL}, 0.0},
                              {{-6LL, 2LL, 3.0}, -1.0},
                              {{-6.0, 2LL, 3.0}, -1.0},
                              // Confirm floating point arithmetic has rounding errors.
                              {{0.1, 0.2}, 0.30000000000000004},
                          });
}

TEST(ExpressionAddTest, Decimals) {
    assertExpectedResults(
        "$add",
        {
            // Singleton case.
            {{Decimal128(1)}, Decimal128(1)},
            // Decimal addition.
            {{Decimal128(1.0), Decimal128(2.0), Decimal128(3.0)}, Decimal128(6.0)},
            {{Decimal128(-6.0), Decimal128(2.0), Decimal128(3.0)}, Decimal128(-1.0)},
            // Confirm that decimals are wider than all other types, and the output
            // will be a double if any operand is a double.
            {{Decimal128(1), 2LL, 3}, Decimal128(6.0)},
            {{Decimal128(3), 2.0, 1LL}, Decimal128(6.0)},
            {{Decimal128(3), 2, 1.0}, Decimal128(6.0)},
            {{1, 2, Decimal128(3.0)}, Decimal128(6.0)},
            {{1LL, Decimal128(2.0), 3.0}, Decimal128(6.0)},
            {{1.0, 2.0, Decimal128(3.0)}, Decimal128(6.0)},
            {{1, Decimal128(2.0), 3.0}, Decimal128(6.0)},
            {{1LL, Decimal128(2.0), 3.0, 2}, Decimal128(8.0)},
            {{1LL, Decimal128(2.0), 3, 2.0}, Decimal128(8.0)},
            {{1, Decimal128(2.0), 3LL, 2.0}, Decimal128(8.0)},
            {{3.0, Decimal128(0.0), 2, 1LL}, Decimal128(6.0)},
            {{1, 3LL, 2.0, Decimal128(2.0)}, Decimal128(8.0)},
            {{3.0, 2, 1LL, Decimal128(0.0)}, Decimal128(6.0)},
            {{Decimal128(-6.0), 2.0, 3LL}, Decimal128(-1.0)},
        });
}

TEST(ExpressionAddTest, DatesNonDecimal) {
    assertExpectedResults(
        "$add",
        {
            {{1, 2, 3, Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(106)},
            {{1LL, 2LL, 3LL, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(106)},
            {{1.0, 2.0, 3.0, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(106)},
            {{1.0, 2.0, Value(Date_t::fromMillisSinceEpoch(100)), 3.0},
             Date_t::fromMillisSinceEpoch(106)},
            {{1.0, 2.2, 3.5, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1, 2.2, 3.5, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1, Date_t::fromMillisSinceEpoch(100), 2.2, 3.5}, Date_t::fromMillisSinceEpoch(107)},
            {{Date_t::fromMillisSinceEpoch(100), 1, 2.2, 3.5}, Date_t::fromMillisSinceEpoch(107)},
            {{-6, Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(94)},
            {{-200, Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(-100)},
            {{1, 2, 3, Date_t::fromMillisSinceEpoch(-100)}, Date_t::fromMillisSinceEpoch(-94)},
        });
}

TEST(ExpressionAddTest, DatesDecimal) {
    assertExpectedResults(
        "$add",
        {
            {{1, Decimal128(2), 3, Date_t::fromMillisSinceEpoch(100)},
             Date_t::fromMillisSinceEpoch(106)},
            {{1LL, 2LL, Decimal128(3LL), Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(106)},
            {{1, Decimal128(2.2), 3.5, Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1, Decimal128(2.2), Decimal128(3.5), Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{1.0, Decimal128(2.2), Decimal128(3.5), Value(Date_t::fromMillisSinceEpoch(100))},
             Date_t::fromMillisSinceEpoch(107)},
            {{Decimal128(-6), Date_t::fromMillisSinceEpoch(100)}, Date_t::fromMillisSinceEpoch(94)},
            {{Decimal128(-200), Date_t::fromMillisSinceEpoch(100)},
             Date_t::fromMillisSinceEpoch(-100)},
            {{1, Decimal128(2), 3, Date_t::fromMillisSinceEpoch(-100)},
             Date_t::fromMillisSinceEpoch(-94)},
        });
}

TEST(ExpressionAddTest, Assertions) {
    // Date addition must fit in a NumberLong from a double.
    ASSERT_THROWS_CODE(
        evaluateExpression("$add", {Date_t::fromMillisSinceEpoch(100), (double)LLONG_MAX}),
        AssertionException,
        ErrorCodes::Overflow);

    // Only one date allowed in an $add expression.
    ASSERT_THROWS_CODE(
        evaluateExpression(
            "$add", {Date_t::fromMillisSinceEpoch(100), 1, Date_t::fromMillisSinceEpoch(100)}),
        AssertionException,
        16612);

    // Only numeric types are allowed in a $add.
    ASSERT_THROWS_CODE(evaluateExpression("$add", {1, 2, "not numeric!"_sd, 3}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}


TEST(ExpressionAddTest, VerifyNoDoubleDoubleSummation) {
    // Confirm that we're not using DoubleDoubleSummation for $add expression with a set of double
    // values from mongo/util/summation_test.cpp.
    std::vector<ImplicitValue> doubleValues = {
        1.4831356930199802e-05,  -3.121724665346865,     3041897608700.073,
        1001318343149.7166,      -1714.6229586696593,    1731390114894580.8,
        6.256645803154374e-08,   -107144114533844.25,    -0.08839485091750919,
        -265119153.02185738,     -0.02450615965231944,   0.0002684331017079073,
        32079040427.68358,       -0.04733295911845742,   0.061381859083076085,
        -25329.59126796951,      -0.0009567520620034965, -1553879364344.9932,
        -2.1101077525869814e-08, -298421079729.5547,     0.03182394834273594,
        22.201944843278916,      -33.35667991109125,     11496013.960449915,
        -40652595.33210472,      3.8496066090328163,     2.5074042398147304e-08,
        -0.02208724071782122,    -134211.37290639878,    0.17640433666616578,
        4.463787499171126,       9.959669945399718,      129265976.35224283,
        1.5865526187526546e-07,  -4746011.710555799,     -712048598925.0789,
        582214206210.4034,       0.025236204812875362,   530078170.91147506,
        -14.865307666195053,     1.6727994895185032e-05, -113386276.03121366,
        -6.135827207137054,      10644945799901.145,     -100848907797.1582,
        2.2404406961625282e-08,  1.315662618424494e-09,  -0.832190208349044,
        -9.779323414999364,      -546522170658.2997};
    double straightSum = 0.0;
    DoubleDoubleSummation compensatedSum;
    for (const auto& x : doubleValues) {
        compensatedSum.addDouble(x.getDouble());
        straightSum += x.getDouble();
    }
    ASSERT_NE(straightSum, compensatedSum.getDouble());

    Value result = evaluateExpression("$add", doubleValues);
    ASSERT_VALUE_EQ(result, Value(straightSum));
    ASSERT_VALUE_NE(result, Value(compensatedSum.getDouble()));
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

// Test we return true if it matches
TEST(ExpressionFLETest, TestBinData) {

    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    {
        auto expr = fromjson(R"({$_internalFleEq: {
        field: {
            "$binary": {
                "base64":
                "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                    "subType": "6"
            }
        },
        server: {
            "$binary": {
                "base64": "CPFLfo1iUCYtRSLiuB+Bt5d1tAe/BCfIfAoGmQLBqBhO",
                "subType": "6"
            }
        }    } })");

        auto exprFle = ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps);

        ASSERT_VALUE_EQ(exprFle->evaluate({}, &expCtx.variables), Value(true));
    }

    {
        auto expr = fromjson(R"({$_internalFleEq: {
        field: {
            "$binary": {
                "base64":
                "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                    "subType": "6"
            }
        },
        server: {
            "$binary": {
                "base64": "CEWSmQID7SfwyAUI3ZkSFkATKryDQfnxXEOGad5d4Rsg",
                "subType": "6"
            }
        }    } })");

        auto exprFle = ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps);

        ASSERT_VALUE_EQ(exprFle->evaluate({}, &expCtx.variables), Value(false));
    }
}

TEST(ExpressionFLETest, TestBinData_RoundTrip) {

    auto expCtx = ExpressionContextForTest();
    auto vps = expCtx.variablesParseState;

    auto expr = fromjson(R"({$_internalFleEq: {
    field: {
        "$binary": {
            "base64":
            "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                "subType": "6"
        }
    },
    server: {
        "$binary": {
            "base64": "CPFLfo1iUCYtRSLiuB+Bt5d1tAe/BCfIfAoGmQLBqBhO",
            "subType": "6"
        }
    }    } })");

    auto exprFle = ExpressionInternalFLEEqual::parse(&expCtx, expr.firstElement(), vps);

    ASSERT_VALUE_EQ(exprFle->evaluate({}, &expCtx.variables), Value(true));

    auto value = exprFle->serialize(false);

    auto roundTripExpr = fromjson(R"({$_internalFleEq: {
    field: {
        "$const" : { "$binary": {
            "base64":
            "DpmKcmnbZ0q1pl/PVwNUh2kCFxinumXuHn6hOSbp+cge6qsJsh7GhSCgRen8HT9JkOZSkZQlSn4IU1vqmTdKRtpk/xX2YJdG76qRYahnyLhl44xjm5Nw1TTMTxAYW3/F0ZTZeWRb2vsU8ICPlHh4xn7isVzmp/0G9k19x67xzboc57gvFXpmCJ3i2qcDAJwaN1fVL/4+S0jJYje8HwgS6qXXaJBCyiZzd31LDXZLWMYkiDvrJBZEMeAnu8gATM5Hg+9Hfte7/C37QED8jjxmAoVB",
                "subType": "6"
        }}
    },
    server: {
        "$binary": {
            "base64": "CPFLfo1iUCYtRSLiuB+Bt5d1tAe/BCfIfAoGmQLBqBhO",
            "subType": "6"
        }
    }    } })");

    ASSERT_BSONOBJ_EQ(value.getDocument().toBson(), roundTripExpr);
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
    auto value = exprFle->serialize(false);

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

/**
 * Expressions registered with REGISTER_EXPRESSION_WITH_FEATURE_FLAG with feature flags that are not
 * active by default are not available for parsing in unit tests, since at MONGO_INITIALIZER-time,
 * the feature flag is false, so the expression isn't registered. This function calls the parse
 * function on an expression class directly to bypass the global parser map.
 */
template <typename T>
Value evaluateUnregisteredExpression(vector<ImplicitValue> operands) {
    auto expCtx = ExpressionContextForTest{};
    auto val = Value(ImplicitValue::convertToValues(operands));
    const BSONObj obj = BSON("" << val);
    auto expr = T::parse(&expCtx, obj.firstElement(), expCtx.variablesParseState);
    return expr->evaluate({}, &expCtx.variables);
}

/**
 * Version of assertExpectedResults() that bypasses the global parser map and always parses
 * expressions of the templated type.
 */
template <typename T>
void assertExpectedResultsUnregistered(
    initializer_list<pair<initializer_list<ImplicitValue>, ImplicitValue>> operations) {
    for (auto&& op : operations) {
        try {
            Value result = evaluateUnregisteredExpression<T>(op.first);
            ASSERT_VALUE_EQ(op.second, result);
            ASSERT_EQUALS(op.second.getType(), result.getType());
        } catch (...) {
            LOGV2(6688000, "failed", "argument"_attr = ImplicitValue::convertToValues(op.first));
            throw;
        }
    }
}

TEST(ExpressionBitAndTest, BitAndCorrectness) {
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Explicit correctness cases.
        {{0b0, 0b0}, 0b0},
        {{0b0, 0b1}, 0b0},
        {{0b1, 0b0}, 0b0},
        {{0b1, 0b1}, 0b1},

        {{0b00, 0b00}, 0b00},
        {{0b00, 0b01}, 0b00},
        {{0b01, 0b00}, 0b00},
        {{0b01, 0b01}, 0b01},

        {{0b00, 0b00}, 0b00},
        {{0b00, 0b11}, 0b00},
        {{0b11, 0b00}, 0b00},
        {{0b11, 0b11}, 0b11},
    });
}

TEST(ExpressionBitAndTest, BitAndInt) {
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Empty operand list should evaluate to the identity for the operation.
        {{}, -1},
        // Singleton cases.
        {{0}, 0},
        {{256}, 256},
        // Binary cases
        {{5, 2}, 5 & 2},
        {{255, 0}, 255 & 0},
        // Ternary cases
        {{5, 2, 10}, 5 & 2 & 10},
    });
}

TEST(ExpressionBitAndTest, BitAndLong) {
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Singleton cases.
        {{0LL}, 0LL},
        {{1LL << 40}, 1LL << 40},
        {{256LL}, 256LL},
        // Binary cases.
        {{5LL, 2LL}, 5LL & 2LL},
        {{255LL, 0LL}, 255LL & 0LL},
        // Ternary cases.
        {{5, 2, 10}, 5 & 2 & 10},
    });
}

TEST(ExpressionBitAndTest, BitAndMixedTypes) {
    // Any NumberLong widens the resulting type to NumberLong.
    assertExpectedResultsUnregistered<ExpressionBitAnd>({
        // Binary cases
        {{5LL, 2}, 5LL & 2},
        {{5, 2LL}, 5 & 2LL},
        {{255LL, 0}, 255LL & 0},
        {{255, 0LL}, 255 & 0LL},
    });
}

TEST(ExpressionBitOrTest, BitOrInt) {
    assertExpectedResultsUnregistered<ExpressionBitOr>({
        {{}, 0},
        // Singleton cases.
        {{0}, 0},
        {{256}, 256},
        // Binary cases
        {{5, 2}, 5 | 2},
        {{255, 0}, 255 | 0},
        // Ternary cases
        {{5, 2, 10}, 5 | 2 | 10},
    });
}

TEST(ExpressionBitOrTest, BitOrLong) {
    assertExpectedResultsUnregistered<ExpressionBitOr>({
        // Singleton cases.
        {{0LL}, 0LL},
        {{256LL}, 256LL},
        // Binary cases.
        {{5LL, 2LL}, 5LL | 2LL},
        {{255LL, 0LL}, 255LL | 0LL},
        // Ternary cases.
        {{5, 2, 10}, 5 | 2 | 10},
    });
}

TEST(ExpressionBitOrTest, BitOrMixedTypes) {
    // Any NumberLong widens the resulting type to NumberLong.
    assertExpectedResultsUnregistered<ExpressionBitOr>({
        // Binary cases
        {{5LL, 2}, 5LL | 2},
        {{5, 2LL}, 5 | 2LL},
        {{255LL, 0}, 255LL | 0},
        {{255, 0LL}, 255 | 0LL},
    });
}

TEST(ExpressionBitXorTest, BitXorInt) {
    assertExpectedResultsUnregistered<ExpressionBitXor>({
        {{}, 0},
        // Singleton cases.
        {{0}, 0},
        {{256}, 256},
        // Binary cases
        {{5, 2}, 5 ^ 2},
        {{255, 0}, 255 ^ 0},
        // Ternary cases
        {{5, 2, 10}, 5 ^ 2 ^ 10},
    });
}

TEST(ExpressionBitXorTest, BitXorLong) {
    assertExpectedResultsUnregistered<ExpressionBitXor>({
        // Singleton cases.
        {{0LL}, 0LL},
        {{256LL}, 256LL},
        // Binary cases.
        {{5LL, 2LL}, 5LL ^ 2LL},
        {{255LL, 0LL}, 255LL ^ 0LL},
        // Ternary cases.
        {{5, 2, 10}, 5 ^ 2 ^ 10},
    });
}

TEST(ExpressionBitXorTest, BitXorMixedTypes) {
    // Any NumberLong widens the resulting type to NumberLong.
    assertExpectedResultsUnregistered<ExpressionBitXor>({
        // Binary cases
        {{5LL, 2}, 5LL ^ 2},
        {{5, 2LL}, 5 ^ 2LL},
        {{255LL, 0}, 255LL ^ 0},
        {{255, 0LL}, 255 ^ 0LL},
    });
}


TEST(ExpressionBitNotTest, Int) {
    int min = numeric_limits<int>::min();
    int max = numeric_limits<int>::max();
    assertExpectedResultsUnregistered<ExpressionBitNot>({
        {{0}, -1},
        {{-1}, 0},
        {{1}, -2},
        {{3}, -4},
        {{100}, -101},
        {{min}, ~min},
        {{max}, ~max},
        {{max}, min},
        {{min}, max},
    });
}

TEST(ExpressionBitNotTest, Long) {
    long long min = numeric_limits<long long>::min();
    long long max = numeric_limits<long long>::max();
    assertExpectedResultsUnregistered<ExpressionBitNot>({
        {{0LL}, -1LL},
        {{-1LL}, 0LL},
        {{1LL}, -2LL},
        {{3LL}, -4LL},
        {{100LL}, -101LL},
        {{2147483649LL}, ~2147483649LL},
        {{-2147483655LL}, ~(-2147483655LL)},
        {{min}, ~min},
        {{max}, ~max},
        {{max}, min},
        {{min}, max},
    });
}

TEST(ExpressionBitNotTest, OtherNumerics) {
    ASSERT_THROWS_CODE(evaluateUnregisteredExpression<ExpressionBitNot>({1.5}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(evaluateUnregisteredExpression<ExpressionBitNot>({Decimal128("0")}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(ExpressionBitNotTest, NonNumerics) {
    ASSERT_THROWS_CODE(
        evaluateUnregisteredExpression<ExpressionBitNot>({"hi"_sd}), AssertionException, 28765);
    ASSERT_THROWS_CODE(
        evaluateUnregisteredExpression<ExpressionBitNot>({true}), AssertionException, 28765);
}

TEST(ExpressionBitNotTest, Arrays) {
    ASSERT_THROWS_CODE(
        evaluateUnregisteredExpression<ExpressionBitNot>({1, 2, 3}), AssertionException, 16020);
    ASSERT_THROWS_CODE(evaluateUnregisteredExpression<ExpressionBitNot>({1LL, 2LL, 3LL}),
                       AssertionException,
                       16020);
}
}  // namespace ExpressionTests
}  // namespace mongo

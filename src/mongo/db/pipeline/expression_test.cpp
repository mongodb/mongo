/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/config.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace ExpressionTests {

using boost::intrusive_ptr;
using std::initializer_list;
using std::numeric_limits;
using std::pair;
using std::set;
using std::sort;
using std::string;
using std::vector;
using std::list;

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
    Value result = expression->evaluate(Document());
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
            log() << "failed with arguments: " << ImplicitValue::convertToValue(op.first);
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
        } else if (str::equals(elem.fieldName(), "$const") ||
                   (elem.type() == mongo::String && elem.valuestrsafe()[0] == '$')) {
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

template <typename T>
intrusive_ptr<ExpressionConstant> makeConstant(T&& val) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    return ExpressionConstant::create(expCtx, Value(std::forward<T>(val)));
}

class ExpressionBaseTest : public unittest::Test {
public:
    void addOperand(intrusive_ptr<ExpressionNary> expr, Value arg) {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        expr->addOperand(ExpressionConstant::create(expCtx, arg));
    }
};

class ExpressionNaryTestOneArg : public ExpressionBaseTest {
public:
    virtual void assertEvaluates(Value input, Value output) {
        addOperand(_expr, input);
        ASSERT_VALUE_EQ(output, _expr->evaluate(Document()));
        ASSERT_EQUALS(output.getType(), _expr->evaluate(Document()).getType());
    }

    intrusive_ptr<ExpressionNary> _expr;
};

/* ------------------------- NaryExpression -------------------------- */

/** A dummy child of ExpressionNary used for testing. */
class Testable : public ExpressionNary {
public:
    virtual Value evaluate(const Document& root) const {
        // Just put all the values in a list.
        // By default, this is not associative/commutative so the results will change if
        // instantiated as commutative or associative and operations are reordered.
        vector<Value> values;
        for (ExpressionVector::const_iterator i = vpOperand.begin(); i != vpOperand.end(); ++i) {
            values.push_back((*i)->evaluate(root));
        }
        return Value(values);
    }

    virtual const char* getOpName() const {
        return "$testable";
    }

    virtual bool isAssociative() const {
        return _isAssociative;
    }

    virtual bool isCommutative() const {
        return _isCommutative;
    }

    static intrusive_ptr<Testable> create(bool associative, bool commutative) {
        return new Testable(associative, commutative);
    }

private:
    Testable(bool isAssociative, bool isCommutative)
        : ExpressionNary(
              boost::intrusive_ptr<ExpressionContextForTest>(new ExpressionContextForTest())),
          _isAssociative(isAssociative),
          _isCommutative(isCommutative) {}
    bool _isAssociative;
    bool _isCommutative;
};

class ExpressionNaryTest : public unittest::Test {
public:
    virtual void setUp() override {
        _notAssociativeNorCommutative = Testable::create(false, false);
        _associativeOnly = Testable::create(true, false);
        _associativeAndCommutative = Testable::create(true, true);
    }

protected:
    void assertDependencies(const intrusive_ptr<Testable>& expr,
                            const BSONArray& expectedDependencies) {
        DepsTracker dependencies;
        expr->addDependencies(&dependencies);
        BSONArrayBuilder dependenciesBson;
        for (set<string>::const_iterator i = dependencies.fields.begin();
             i != dependencies.fields.end();
             ++i) {
            dependenciesBson << *i;
        }
        ASSERT_BSONOBJ_EQ(expectedDependencies, dependenciesBson.arr());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }

    void assertContents(const intrusive_ptr<Testable>& expr, const BSONArray& expectedContents) {
        ASSERT_BSONOBJ_EQ(constify(BSON("$testable" << expectedContents)), expressionToBson(expr));
    }

    void addOperandArrayToExpr(const intrusive_ptr<Testable>& expr, const BSONArray& operands) {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        VariablesParseState vps = expCtx->variablesParseState;
        BSONObjIterator i(operands);
        while (i.more()) {
            BSONElement element = i.next();
            expr->addOperand(Expression::parseOperand(expCtx, element, vps));
        }
    }

    intrusive_ptr<Testable> _notAssociativeNorCommutative;
    intrusive_ptr<Testable> _associativeOnly;
    intrusive_ptr<Testable> _associativeAndCommutative;
};

TEST_F(ExpressionNaryTest, AddedConstantOperandIsSerialized) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(expCtx, Value(9)));
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY(9));
}

TEST_F(ExpressionNaryTest, AddedFieldPathOperandIsSerialized) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    _notAssociativeNorCommutative->addOperand(ExpressionFieldPath::create(expCtx, "ab.c"));
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY("$ab.c"));
}

TEST_F(ExpressionNaryTest, ValidateEmptyDependencies) {
    assertDependencies(_notAssociativeNorCommutative, BSONArray());
}

TEST_F(ExpressionNaryTest, ValidateConstantExpressionDependency) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(expCtx, Value(1)));
    assertDependencies(_notAssociativeNorCommutative, BSONArray());
}

TEST_F(ExpressionNaryTest, ValidateFieldPathExpressionDependency) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    _notAssociativeNorCommutative->addOperand(ExpressionFieldPath::create(expCtx, "ab.c"));
    assertDependencies(_notAssociativeNorCommutative, BSON_ARRAY("ab.c"));
}

TEST_F(ExpressionNaryTest, ValidateObjectExpressionDependency) {
    BSONObj spec = BSON("" << BSON("a"
                                   << "$x"
                                   << "q"
                                   << "$r"));
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONElement specElement = spec.firstElement();
    VariablesParseState vps = expCtx->variablesParseState;
    _notAssociativeNorCommutative->addOperand(
        Expression::parseObject(expCtx, specElement.Obj(), vps));
    assertDependencies(_notAssociativeNorCommutative,
                       BSON_ARRAY("r"
                                  << "x"));
}

TEST_F(ExpressionNaryTest, SerializationToBsonObj) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(expCtx, Value(5)));
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5)))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(false)));
}

TEST_F(ExpressionNaryTest, SerializationToBsonArr) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(expCtx, Value(5)));
    ASSERT_BSONOBJ_EQ(constify(BSON_ARRAY(BSON("$testable" << BSON_ARRAY(5)))),
                      BSON_ARRAY(_notAssociativeNorCommutative->serialize(false)));
}

// Verify that the internal operands are optimized
TEST_F(ExpressionNaryTest, InternalOperandOptimizationIsDone) {
    BSONArray spec = BSON_ARRAY(BSON("$and" << BSONArray()) << "$abc");
    addOperandArrayToExpr(_notAssociativeNorCommutative, spec);
    assertContents(_notAssociativeNorCommutative, spec);
    ASSERT(_notAssociativeNorCommutative == _notAssociativeNorCommutative->optimize());
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY(true << "$abc"));
}

// Verify that if all the operands are constants, the expression is replaced
// by a constant value equivalent to the expression applied to the operands.
TEST_F(ExpressionNaryTest, AllConstantOperandOptimization) {
    BSONArray spec = BSON_ARRAY(1 << 2);
    addOperandArrayToExpr(_notAssociativeNorCommutative, spec);
    assertContents(_notAssociativeNorCommutative, spec);
    intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative != optimized);
    ASSERT_BSONOBJ_EQ(BSON("$const" << BSON_ARRAY(1 << 2)), expressionToBson(optimized));
}

// Verify that the optimization of grouping constant and non-constant operands
// and then applying the expression to the constant operands to reduce them to
// one constant operand is only applied if the expression is associative and
// commutative.
TEST_F(ExpressionNaryTest, GroupingOptimizationOnNotCommutativeNorAssociative) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_notAssociativeNorCommutative, spec);
    assertContents(_notAssociativeNorCommutative, spec);
    intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative == optimized);
    assertContents(_notAssociativeNorCommutative, spec);
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyFrontOperands) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyMiddleOperands) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyBackOperands) {
    BSONArray spec = BSON_ARRAY("$path" << 55 << 66);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsFront) {
    BSONArray spec = BSON_ARRAY(55 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY(55 << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsMiddle) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << "$path2");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << 55 << "$path2"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsBack) {
    BSONArray spec = BSON_ARRAY("$path" << 55);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << 55));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnCommutativeAndAssociative) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_associativeAndCommutative, spec);
    assertContents(_associativeAndCommutative, spec);
    intrusive_ptr<Expression> optimized = _associativeAndCommutative->optimize();
    ASSERT(_associativeAndCommutative == optimized);
    assertContents(_associativeAndCommutative, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, FlattenOptimizationNotDoneOnOtherExpressionsForAssociativeExpressions) {
    BSONArray spec = BSON_ARRAY(66 << "$path" << BSON("$sum" << BSON_ARRAY("$path" << 2)));
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, spec);
}

TEST_F(ExpressionNaryTest, FlattenOptimizationNotDoneOnSameButNotAssociativeExpression) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(false, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    assertContents(_associativeOnly, spec);
}

// Test that if there is an expression of the same type in a non-commutative nor associative
// expression, the inner expression is not expanded.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnNotCommutativeNorAssociative) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(false, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _notAssociativeNorCommutative->addOperand(innerOperand);

    addOperandArrayToExpr(_notAssociativeNorCommutative, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_notAssociativeNorCommutative, spec);
    intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative == optimized);

    assertContents(_notAssociativeNorCommutative, spec);
}

// Test that if there is an expression of the same type as the first operand
// in a non-commutative but associative expression, the inner expression is expanded.
// Also, there shouldn't be any grouping of the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ 100, "$path1", 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyFrontOperandNoGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << 99 << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type as the first operand
// in a non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1", 101] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ 100, "$path1", [101, 99], "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyFrontOperandAndGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << BSON_ARRAY(101 << 99) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the middle of the operands
// in a non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should not be applied to the operands.
// {"$testable" : [ 200, "$path3", { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is
// optimized to: {"$testable" : [ 200, "$path3", 100, "$path1", 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyMiddleOperandNoGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3"));
    specBuilder << 200 << "$path3";

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(200 << "$path3" << 100 << "$path1" << 99 << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the middle of the operands
// in a non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ 200, "$path3", 201 { "$testable" : [ 100, "$path1", 101] }, 99, "$path2"] } is
// optimized to: {"$testable" : [ 200, "$path3", [201, 100], "$path1", [101, 99], "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyMiddleOperandAndGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3" << 201));
    specBuilder << 200 << "$path3" << 201;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(
        200 << "$path3" << BSON_ARRAY(201 << 100) << "$path1" << BSON_ARRAY(101 << 99) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the back of the operands in a
// non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should not be applied to the operands.
// {"$testable" : [ 200, "$path3", { "$testable" : [ 100, "$path1"] }] } is
// optimized to: {"$testable" : [ 200, "$path3", 100, "$path1"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyBackOperandNoGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3"));
    specBuilder << 200 << "$path3";

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(200 << "$path3" << 100 << "$path1");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there is an expression of the same type in the back of the operands in a
// non-commutative but associative expression, the inner expression is expanded.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ 200, "$path3", 201, { "$testable" : [ 100, "$path1", 101] }] } is
// optimized to: {"$testable" : [ 200, "$path3", [201, 100], "$path1", 101] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnAssociativeOnlyBackOperandAndGroup) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(200 << "$path3" << 201));
    specBuilder << 200 << "$path3" << 201;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent =
        BSON_ARRAY(200 << "$path3" << BSON_ARRAY(201 << 100) << "$path1" << 101);
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there are two consecutive inner expressions of the same type in a non-commutative
// but associative expression, both expressions are correctly flattened.
// Partial collapsing optimization should not be applied to the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, { "$testable" : [ 200, "$path2"] }] } is
// optimized to: {"$testable" : [ 100, "$path1", 200, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenConsecutiveInnerOperandsOptimizationOnAssociativeOnlyNoGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    intrusive_ptr<Testable> innerOperand2 = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand2, BSON_ARRAY(200 << "$path2"));
    specBuilder.append(expressionToBson(innerOperand2));
    _associativeOnly->addOperand(innerOperand2);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << 200 << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that if there are two consecutive inner expressions of the same type in a non-commutative
// but associative expression, both expressions are correctly flattened.
// Partial collapsing optimization should be applied to the operands.
// {"$testable" : [ { "$testable" : [ 100, "$path1", 101] }, { "$testable" : [ 200, "$path2"] }] }
// is optimized to: {"$testable" : [ 100, "$path1", [ 101, 200], "$path2"] }
TEST_F(ExpressionNaryTest, FlattenConsecutiveInnerOperandsOptimizationOnAssociativeAndGroup) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    intrusive_ptr<Testable> innerOperand2 = Testable::create(true, false);
    addOperandArrayToExpr(innerOperand2, BSON_ARRAY(200 << "$path2"));
    specBuilder.append(expressionToBson(innerOperand2));
    _associativeOnly->addOperand(innerOperand2);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << BSON_ARRAY(101 << 200) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}

// Test that inner expressions are correctly flattened and constant operands re-arranged and
// collapsed when using a commutative and associative expression.
// {"$testable" : [ 200, "$path3", 201, { "$testable" : [ 100, "$path1", 101] }, 99, "$path2"] } is
// optimized to: {"$testable" : [ "$path3", "$path1", "$path2", [200, 201, [ 100, 101], 99] ] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnCommutativeAndAssociative) {
    BSONArrayBuilder specBuilder;

    addOperandArrayToExpr(_associativeAndCommutative, BSON_ARRAY(200 << "$path3" << 201));
    specBuilder << 200 << "$path3" << 201;

    intrusive_ptr<Testable> innerOperand = Testable::create(true, true);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeAndCommutative->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeAndCommutative, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeAndCommutative, spec);
    intrusive_ptr<Expression> optimized = _associativeAndCommutative->optimize();
    ASSERT(_associativeAndCommutative == optimized);

    BSONArray expectedContent = BSON_ARRAY("$path3"
                                           << "$path1"
                                           << "$path2"
                                           << BSON_ARRAY(200 << 201 << BSON_ARRAY(100 << 101)
                                                             << 99));
    assertContents(_associativeAndCommutative, expectedContent);
}

/* ------------------------- ExpressionCeil -------------------------- */

class ExpressionCeilTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        _expr = new ExpressionCeil(expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionCeilTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(numeric_limits<int>::min()), Value(numeric_limits<int>::min()));
    assertEvaluates(Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max()));
}

TEST_F(ExpressionCeilTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(numeric_limits<long long>::min()),
                    Value(numeric_limits<long long>::min()));
    assertEvaluates(Value(numeric_limits<long long>::max()),
                    Value(numeric_limits<long long>::max()));
}

TEST_F(ExpressionCeilTest, DoubleArg) {
    assertEvaluates(Value(2.0), Value(2.0));
    assertEvaluates(Value(-2.0), Value(-2.0));
    assertEvaluates(Value(0.9), Value(1.0));
    assertEvaluates(Value(0.1), Value(1.0));
    assertEvaluates(Value(-1.2), Value(-1.0));
    assertEvaluates(Value(-1.7), Value(-1.0));

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // ceil should just preserve the number).
    double largerThanLong = numeric_limits<long long>::max() * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEvaluates(Value(smallerThanLong), Value(smallerThanLong));
}

TEST_F(ExpressionCeilTest, DecimalArg) {
    assertEvaluates(Value(Decimal128("2")), Value(Decimal128("2.0")));
    assertEvaluates(Value(Decimal128("-2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("0.9")), Value(Decimal128("1.0")));
    assertEvaluates(Value(Decimal128("0.1")), Value(Decimal128("1.0")));
    assertEvaluates(Value(Decimal128("-1.2")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("-1.7")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("1234567889.000000000000000000000001")),
                    Value(Decimal128("1234567890")));
    assertEvaluates(Value(Decimal128("-99999999999999999999999999999.99")),
                    Value(Decimal128("-99999999999999999999999999999.00")));
    assertEvaluates(Value(Decimal128("3.4E-6000")), Value(Decimal128("1")));
}

TEST_F(ExpressionCeilTest, NullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
}

/* ------------------------- ExpressionFloor -------------------------- */

class ExpressionFloorTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        _expr = new ExpressionFloor(expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionFloorTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(numeric_limits<int>::min()), Value(numeric_limits<int>::min()));
    assertEvaluates(Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max()));
}

TEST_F(ExpressionFloorTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(numeric_limits<long long>::min()),
                    Value(numeric_limits<long long>::min()));
    assertEvaluates(Value(numeric_limits<long long>::max()),
                    Value(numeric_limits<long long>::max()));
}

TEST_F(ExpressionFloorTest, DoubleArg) {
    assertEvaluates(Value(2.0), Value(2.0));
    assertEvaluates(Value(-2.0), Value(-2.0));
    assertEvaluates(Value(0.9), Value(0.0));
    assertEvaluates(Value(0.1), Value(0.0));
    assertEvaluates(Value(-1.2), Value(-2.0));
    assertEvaluates(Value(-1.7), Value(-2.0));

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // floor should just preserve the number).
    double largerThanLong = numeric_limits<long long>::max() * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEvaluates(Value(smallerThanLong), Value(smallerThanLong));
}

TEST_F(ExpressionFloorTest, DecimalArg) {
    assertEvaluates(Value(Decimal128("2")), Value(Decimal128("2.0")));
    assertEvaluates(Value(Decimal128("-2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("0.9")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("0.1")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("-1.2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("-1.7")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("1234567890.000000000000000000000001")),
                    Value(Decimal128("1234567890")));
    assertEvaluates(Value(Decimal128("-99999999999999999999999999999.99")),
                    Value(Decimal128("-100000000000000000000000000000")));
    assertEvaluates(Value(Decimal128("3.4E-6000")), Value(Decimal128("0")));
}

TEST_F(ExpressionFloorTest, NullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
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

/* ------------------------- ExpressionTrunc -------------------------- */

class ExpressionTruncTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        _expr = new ExpressionTrunc(expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionTruncTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(numeric_limits<int>::min()), Value(numeric_limits<int>::min()));
    assertEvaluates(Value(numeric_limits<int>::max()), Value(numeric_limits<int>::max()));
}

TEST_F(ExpressionTruncTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(numeric_limits<long long>::min()),
                    Value(numeric_limits<long long>::min()));
    assertEvaluates(Value(numeric_limits<long long>::max()),
                    Value(numeric_limits<long long>::max()));
}

TEST_F(ExpressionTruncTest, DoubleArg) {
    assertEvaluates(Value(2.0), Value(2.0));
    assertEvaluates(Value(-2.0), Value(-2.0));
    assertEvaluates(Value(0.9), Value(0.0));
    assertEvaluates(Value(0.1), Value(0.0));
    assertEvaluates(Value(-1.2), Value(-1.0));
    assertEvaluates(Value(-1.7), Value(-1.0));

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // should just preserve the number).
    double largerThanLong = numeric_limits<long long>::max() * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEvaluates(Value(smallerThanLong), Value(smallerThanLong));
}

TEST_F(ExpressionTruncTest, DecimalArg) {
    assertEvaluates(Value(Decimal128("2")), Value(Decimal128("2.0")));
    assertEvaluates(Value(Decimal128("-2")), Value(Decimal128("-2.0")));
    assertEvaluates(Value(Decimal128("0.9")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("0.1")), Value(Decimal128("0.0")));
    assertEvaluates(Value(Decimal128("-1.2")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("-1.7")), Value(Decimal128("-1.0")));
    assertEvaluates(Value(Decimal128("123456789.9999999999999999999999999")),
                    Value(Decimal128("123456789")));
    assertEvaluates(Value(Decimal128("-99999999999999999999999999999.99")),
                    Value(Decimal128("-99999999999999999999999999999.00")));
    assertEvaluates(Value(Decimal128("3.4E-6000")), Value(Decimal128("0")));
}

TEST_F(ExpressionTruncTest, NullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
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
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(expression->evaluate(Document())));
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
        ASSERT_BSONOBJ_EQ(BSON("" << 2), toBson(expression->evaluate(Document())));
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
        ASSERT_THROWS(expression->evaluate(Document()), AssertionException);
    }
};

/** Bool type unsupported. */
class Bool {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<ExpressionNary> expression = new ExpressionAdd(expCtx);
        expression->addOperand(ExpressionConstant::create(expCtx, Value(true)));
        ASSERT_THROWS(expression->evaluate(Document()), AssertionException);
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

namespace And {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate(fromBson(BSON("a" << 1)))));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(optimized->evaluate(fromBson(BSON("a" << 1)))));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual bool expectedResult() = 0;
};

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(expectedOptimized(), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class NoOptimizeBase : public OptimizeBase {
    BSONObj expectedOptimized() {
        return constify(spec());
    }
};

/** $and without operands. */
class NoOperands : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSONArray());
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'true'. */
class True : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'false'. */
class False : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'true', 'true'. */
class TrueTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'true', 'false'. */
class TrueFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'false', 'true'. */
class FalseTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(false << true));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'false', 'false'. */
class FalseFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(false << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed 'true', 'true', 'true'. */
class TrueTrueTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << true << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed 'true', 'true', 'false'. */
class TrueTrueFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(true << true << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed '0', '1'. */
class ZeroOne : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(0 << 1));
    }
    bool expectedResult() {
        return false;
    }
};

/** $and passed '1', '2'. */
class OneTwo : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << 2));
    }
    bool expectedResult() {
        return true;
    }
};

/** $and passed a field path. */
class FieldPath : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
    bool expectedResult() {
        return true;
    }
};

/** A constant expression is optimized to a constant. */
class OptimizeConstantExpression : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** A non constant expression is not optimized. */
class NonConstant : public NoOptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantTrue : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
    // note: using $and as serialization of ExpressionCoerceToBool rather than
    // ExpressionAnd
};

class ConstantNonConstantFalse : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(0 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with a field path and '1'. */
class NonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a" << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression with a field path and '0'. */
class NonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a" << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with two field paths and '1'. */
class NonConstantNonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"
                                         << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"));
    }
};

/** An expression with two field paths and '0'. */
class NonConstantNonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"
                                         << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with '0', '1', and a field path. */
class ZeroOneNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(0 << 1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

/** An expression with '1', '1', and a field path. */
class OneOneNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << 1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** Nested $and expressions. */
class Nested : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(1 << BSON("$and" << BSON_ARRAY(1)) << "$a"
                                           << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"
                                         << "$b"));
    }
};

/** Nested $and expressions containing a nested value evaluating to false. */
class NestedZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$and" << BSON_ARRAY(
                        1 << BSON("$and" << BSON_ARRAY(BSON("$and" << BSON_ARRAY(0)))) << "$a"
                          << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << false);
    }
};

}  // namespace And

namespace CoerceToBool {

/** Nested expression coerced to true. */
class EvaluateTrue {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> nested = ExpressionConstant::create(expCtx, Value(5));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(expCtx, nested);
        ASSERT(expression->evaluate(Document()).getBool());
    }
};

/** Nested expression coerced to false. */
class EvaluateFalse {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> nested = ExpressionConstant::create(expCtx, Value(0));
        intrusive_ptr<Expression> expression = ExpressionCoerceToBool::create(expCtx, nested);
        ASSERT(!expression->evaluate(Document()).getBool());
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
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
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

namespace Compare {

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(constify(expectedOptimized()), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class FieldRangeOptimize : public OptimizeBase {
    BSONObj expectedOptimized() {
        return spec();
    }
};

class NoOptimize : public OptimizeBase {
    BSONObj expectedOptimized() {
        return spec();
    }
};

/** Check expected result for expressions depending on constants. */
class ExpectedResultBase : public OptimizeBase {
public:
    void run() {
        OptimizeBase::run();
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        // Check expression spec round trip.
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        // Check evaluation result.
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(expression->evaluate(Document())));
        // Check that the result is the same after optimizing.
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(optimized->evaluate(Document())));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedResult() = 0;

private:
    virtual BSONObj expectedOptimized() {
        return BSON("$const" << expectedResult().firstElement());
    }
};

class ExpectedTrue : public ExpectedResultBase {
    BSONObj expectedResult() {
        return BSON("" << true);
    }
};

class ExpectedFalse : public ExpectedResultBase {
    BSONObj expectedResult() {
        return BSON("" << false);
    }
};

class ParseError {
public:
    virtual ~ParseError() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        ASSERT_THROWS(Expression::parseOperand(expCtx, specElement, vps), AssertionException);
    }

protected:
    virtual BSONObj spec() = 0;
};

/** $eq with first < second. */
class EqLt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 2));
    }
};

/** $eq with first == second. */
class EqEq : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 1));
    }
};

/** $eq with first > second. */
class EqGt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 0));
    }
};

/** $ne with first < second. */
class NeLt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << 2));
    }
};

/** $ne with first == second. */
class NeEq : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << 1));
    }
};

/** $ne with first > second. */
class NeGt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << 0));
    }
};

/** $gt with first < second. */
class GtLt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(1 << 2));
    }
};

/** $gt with first == second. */
class GtEq : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(1 << 1));
    }
};

/** $gt with first > second. */
class GtGt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(1 << 0));
    }
};

/** $gte with first < second. */
class GteLt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(1 << 2));
    }
};

/** $gte with first == second. */
class GteEq : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(1 << 1));
    }
};

/** $gte with first > second. */
class GteGt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(1 << 0));
    }
};

/** $lt with first < second. */
class LtLt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << 2));
    }
};

/** $lt with first == second. */
class LtEq : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << 1));
    }
};

/** $lt with first > second. */
class LtGt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << 0));
    }
};

/** $lte with first < second. */
class LteLt : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(1 << 2));
    }
};

/** $lte with first == second. */
class LteEq : public ExpectedTrue {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(1 << 1));
    }
};

/** $lte with first > second. */
class LteGt : public ExpectedFalse {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(1 << 0));
    }
};

/** $cmp with first < second. */
class CmpLt : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << 2));
    }
    BSONObj expectedResult() {
        return BSON("" << -1);
    }
};

/** $cmp with first == second. */
class CmpEq : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << 1));
    }
    BSONObj expectedResult() {
        return BSON("" << 0);
    }
};

/** $cmp with first > second. */
class CmpGt : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << 0));
    }
    BSONObj expectedResult() {
        return BSON("" << 1);
    }
};

/** $cmp results are bracketed to an absolute value of 1. */
class CmpBracketed : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY("z"
                                         << "a"));
    }
    BSONObj expectedResult() {
        return BSON("" << 1);
    }
};

/** Zero operands provided. */
class ZeroOperands : public ParseError {
    BSONObj spec() {
        return BSON("$ne" << BSONArray());
    }
};

/** One operand provided. */
class OneOperand : public ParseError {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1));
    }
};

/** Three operands provided. */
class ThreeOperands : public ParseError {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(2 << 3 << 4));
    }
};

/** Incompatible types can be compared. */
class IncompatibleTypes {
public:
    void run() {
        BSONObj specObject = BSON("" << BSON("$ne" << BSON_ARRAY("a" << 1)));
        BSONElement specElement = specObject.firstElement();
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_VALUE_EQ(expression->evaluate(Document()), Value(true));
    }
};

/**
 * An expression depending on constants is optimized to a constant via
 * ExpressionNary::optimize().
 */
class OptimizeConstants : public OptimizeBase {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** $cmp is not optimized. */
class NoOptimizeCmp : public NoOptimize {
    BSONObj spec() {
        return BSON("$cmp" << BSON_ARRAY(1 << "$a"));
    }
};

/** $ne is not optimized. */
class NoOptimizeNe : public NoOptimize {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY(1 << "$a"));
    }
};

/** No optimization is performend without a constant. */
class NoOptimizeNoConstant : public NoOptimize {
    BSONObj spec() {
        return BSON("$ne" << BSON_ARRAY("$a"
                                        << "$b"));
    }
};

/** No optimization is performend without an immediate field path. */
class NoOptimizeWithoutFieldPath : public NoOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(BSON("$and" << BSON_ARRAY("$a")) << 1));
    }
};

/** No optimization is performend without an immediate field path. */
class NoOptimizeWithoutFieldPathReverse : public NoOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << BSON("$and" << BSON_ARRAY("$a"))));
    }
};

/** An equality expression is optimized. */
class OptimizeEq : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY("$a" << 1));
    }
};

/** A reverse sense equality expression is optimized. */
class OptimizeEqReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$eq" << BSON_ARRAY(1 << "$a"));
    }
};

/** A $lt expression is optimized. */
class OptimizeLt : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY("$a" << 1));
    }
};

/** A reverse sense $lt expression is optimized. */
class OptimizeLtReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lt" << BSON_ARRAY(1 << "$a"));
    }
};

/** A $lte expression is optimized. */
class OptimizeLte : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $lte expression is optimized. */
class OptimizeLteReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$lte" << BSON_ARRAY(2 << "$b"));
    }
};

/** A $gt expression is optimized. */
class OptimizeGt : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $gt expression is optimized. */
class OptimizeGtReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gt" << BSON_ARRAY(2 << "$b"));
    }
};

/** A $gte expression is optimized. */
class OptimizeGte : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $gte expression is optimized. */
class OptimizeGteReverse : public FieldRangeOptimize {
    BSONObj spec() {
        return BSON("$gte" << BSON_ARRAY(2 << "$b"));
    }
};

}  // namespace Compare

namespace Constant {

/** Create an ExpressionConstant from a Value. */
class Create {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionConstant::create(expCtx, Value(5));
        assertBinaryEqual(BSON("" << 5), toBson(expression->evaluate(Document())));
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
                          toBson(expression->evaluate(Document())));
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
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
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
    assertBinaryEqual(BSONObj(), toBson(expression->evaluate(Document{{"foo", Value("bar"_sd)}})));
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

namespace FieldPath {

/** The provided field path does not pass validation. */
class Invalid {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        ASSERT_THROWS(ExpressionFieldPath::create(expCtx, ""), AssertionException);
    }
};

TEST(FieldPath, NoOptimizationForRootFieldPathWithDottedPath) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    intrusive_ptr<ExpressionFieldPath> expression =
        ExpressionFieldPath::parse(expCtx, "$$ROOT.x.y", expCtx->variablesParseState);

    // An attempt to optimize returns the Expression itself.
    ASSERT_EQUALS(expression, expression->optimize());
}

TEST(FieldPath, NoOptimizationForCurrentFieldPathWithDottedPath) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    intrusive_ptr<ExpressionFieldPath> expression =
        ExpressionFieldPath::parse(expCtx, "$$CURRENT.x.y", expCtx->variablesParseState);

    // An attempt to optimize returns the Expression itself.
    ASSERT_EQUALS(expression, expression->optimize());
}

TEST(FieldPath, RemoveOptimizesToMissingValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    intrusive_ptr<ExpressionFieldPath> expression =
        ExpressionFieldPath::parse(expCtx, "$$REMOVE", expCtx->variablesParseState);

    auto optimizedExpr = expression->optimize();

    ASSERT_VALUE_EQ(Value(), optimizedExpr->evaluate(Document(BSON("x" << BSON("y" << 123)))));
}

TEST(FieldPath, NoOptimizationOnNormalPath) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a");
    // An attempt to optimize returns the Expression itself.
    ASSERT_EQUALS(expression, expression->optimize());
}

TEST(FieldPath, OptimizeOnVariableWithConstantScalarValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    expCtx->variables.setConstantValue(varId, Value(123));

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    ASSERT_TRUE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
}

TEST(FieldPath, OptimizeOnVariableWithConstantArrayValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    expCtx->variables.setConstantValue(varId, Value(BSON_ARRAY(1 << 2 << 3)));

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), constantExpr->getValue());
}

TEST(FieldPath, OptimizeToEmptyArrayOnNumericalPathComponentAndConstantArrayValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    expCtx->variables.setConstantValue(varId, Value(BSON_ARRAY(1 << 2 << 3)));

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar.1", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(BSONArray()), constantExpr->getValue());
}

TEST(FieldPath, OptimizeOnVariableWithConstantValueAndDottedPath) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    expCtx->variables.setConstantValue(varId, Value(Document{{"x", Document{{"y", 123}}}}));

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar.x.y", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(123), constantExpr->getValue());
}

TEST(FieldPath, NoOptimizationOnVariableWithNoValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->variablesParseState.defineVariable("userVar");

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
}

TEST(FieldPath, NoOptimizationOnVariableWithMissingValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    expCtx->variables.setValue(varId, Value());

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(optimizedExpr.get()));
}

TEST(FieldPath, ScalarVariableWithDottedFieldPathOptimizesToConstantMissingValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varId = expCtx->variablesParseState.defineVariable("userVar");
    expCtx->variables.setConstantValue(varId, Value(123));

    auto expr = ExpressionFieldPath::parse(expCtx, "$$userVar.x.y", expCtx->variablesParseState);
    ASSERT_TRUE(dynamic_cast<ExpressionFieldPath*>(expr.get()));

    auto optimizedExpr = expr->optimize();
    auto constantExpr = dynamic_cast<ExpressionConstant*>(optimizedExpr.get());
    ASSERT_TRUE(constantExpr);
    ASSERT_VALUE_EQ(Value(), constantExpr->getValue());
}

/** The field path itself is a dependency. */
class Dependencies {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        DepsTracker dependencies;
        expression->addDependencies(&dependencies);
        ASSERT_EQUALS(1U, dependencies.fields.size());
        ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedTextScore());
    }
};

/** Field path target field is missing. */
class Missing {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a");
        assertBinaryEqual(fromjson("{}"), toBson(expression->evaluate(Document())));
    }
};

/** Simple case where the target field is present. */
class Present {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a");
        assertBinaryEqual(fromjson("{'':123}"),
                          toBson(expression->evaluate(fromBson(BSON("a" << 123)))));
    }
};

/** Target field parent is null. */
class NestedBelowNull {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:null}")))));
    }
};

/** Target field parent is undefined. */
class NestedBelowUndefined {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:undefined}")))));
    }
};

/** Target field parent is missing. */
class NestedBelowMissing {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(fromjson("{z:1}")))));
    }
};

/** Target field parent is an integer. */
class NestedBelowInt {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{}"), toBson(expression->evaluate(fromBson(BSON("a" << 2)))));
    }
};

/** A value in a nested object. */
class NestedValue {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(BSON("" << 55),
                          toBson(expression->evaluate(fromBson(BSON("a" << BSON("b" << 55))))));
    }
};

/** Target field within an empty object. */
class NestedBelowEmptyObject {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{}"),
                          toBson(expression->evaluate(fromBson(BSON("a" << BSONObj())))));
    }
};

/** Target field within an empty array. */
class NestedBelowEmptyArray {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(BSON("" << BSONArray()),
                          toBson(expression->evaluate(fromBson(BSON("a" << BSONArray())))));
    }
};

/** Target field within an array containing null. */
class NestedBelowArrayWithNull {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{'':[]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[null]}")))));
    }
};

/** Target field within an array containing undefined. */
class NestedBelowArrayWithUndefined {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{'':[]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[undefined]}")))));
    }
};

/** Target field within an array containing an integer. */
class NestedBelowArrayWithInt {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{'':[]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[1]}")))));
    }
};

/** Target field within an array. */
class NestedWithinArray {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{'':[9]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[{b:9}]}")))));
    }
};

/** Multiple value types within an array. */
class MultipleArrayValues {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b");
        assertBinaryEqual(fromjson("{'':[9,20]}"),
                          toBson(expression->evaluate(
                              fromBson(fromjson("{a:[{b:9},null,undefined,{g:4},{b:20},{}]}")))));
    }
};

/** Expanding values within nested arrays. */
class ExpandNestedArrays {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b.c");
        assertBinaryEqual(fromjson("{'':[[1,2],3,[4],[[5]],[6,7]]}"),
                          toBson(expression->evaluate(fromBson(fromjson("{a:[{b:[{c:1},{c:2}]},"
                                                                        "{b:{c:3}},"
                                                                        "{b:[{c:4}]},"
                                                                        "{b:[{c:[5]}]},"
                                                                        "{b:{c:[6,7]}}]}")))));
    }
};

/** Add to a BSONObj. */
class AddToBsonObj {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b.c");
        assertBinaryEqual(BSON("foo"
                               << "$a.b.c"),
                          BSON("foo" << expression->serialize(false)));
    }
};

/** Add to a BSONArray. */
class AddToBsonArray {
public:
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        intrusive_ptr<Expression> expression = ExpressionFieldPath::create(expCtx, "a.b.c");
        BSONArrayBuilder bab;
        bab << expression->serialize(false);
        assertBinaryEqual(BSON_ARRAY("$a.b.c"), bab.arr());
    }
};

}  // namespace FieldPath

namespace Object {
using mongo::ExpressionObject;

template <typename T>
Document literal(T&& value) {
    return Document{{"$const", Value(std::forward<T>(value))}};
}

//
// Parsing.
//

TEST(ExpressionObjectParse, ShouldAcceptEmptyObject) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object = ExpressionObject::parse(expCtx, BSONObj(), vps);
    ASSERT_VALUE_EQ(Value(Document{}), object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptLiteralsAsValues) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object = ExpressionObject::parse(expCtx,
                                          BSON("a" << 5 << "b"
                                                   << "string"
                                                   << "c"
                                                   << BSONNULL),
                                          vps);
    auto expectedResult =
        Value(Document{{"a", literal(5)}, {"b", literal("string"_sd)}, {"c", literal(BSONNULL)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAccept_idAsFieldName) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object = ExpressionObject::parse(expCtx, BSON("_id" << 5), vps);
    auto expectedResult = Value(Document{{"_id", literal(5)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptFieldNameContainingDollar) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object = ExpressionObject::parse(expCtx, BSON("a$b" << 5), vps);
    auto expectedResult = Value(Document{{"a$b", literal(5)}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptNestedObjects) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object =
        ExpressionObject::parse(expCtx, fromjson("{a: {b: 1}, c: {d: {e: 1, f: 1}}}"), vps);
    auto expectedResult =
        Value(Document{{"a", Document{{"b", literal(1)}}},
                       {"c", Document{{"d", Document{{"e", literal(1)}, {"f", literal(1)}}}}}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ExpressionObjectParse, ShouldAcceptArrays) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object = ExpressionObject::parse(expCtx, fromjson("{a: [1, 2]}"), vps);
    auto expectedResult =
        Value(Document{{"a", vector<Value>{Value(literal(1)), Value(literal(2))}}});
    ASSERT_VALUE_EQ(expectedResult, object->serialize(false));
}

TEST(ObjectParsing, ShouldAcceptExpressionAsValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto object = ExpressionObject::parse(expCtx, BSON("a" << BSON("$and" << BSONArray())), vps);
    ASSERT_VALUE_EQ(object->serialize(false),
                    Value(Document{{"a", Document{{"$and", BSONArray()}}}}));
}

//
// Error cases.
//

TEST(ExpressionObjectParse, ShouldRejectDottedFieldNames) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("a.b" << 1), vps), AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("c" << 3 << "a.b" << 1), vps),
                  AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("a.b" << 1 << "c" << 3), vps),
                  AssertionException);
}

TEST(ExpressionObjectParse, ShouldRejectDuplicateFieldNames) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("a" << 1 << "a" << 1), vps),
                  AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("a" << 1 << "b" << 2 << "a" << 1), vps),
                  AssertionException);
    ASSERT_THROWS(
        ExpressionObject::parse(expCtx, BSON("a" << BSON("c" << 1) << "b" << 2 << "a" << 1), vps),
        AssertionException);
    ASSERT_THROWS(
        ExpressionObject::parse(expCtx, BSON("a" << 1 << "b" << 2 << "a" << BSON("c" << 1)), vps),
        AssertionException);
}

TEST(ExpressionObjectParse, ShouldRejectInvalidFieldName) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("$a" << 1), vps), AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON("" << 1), vps), AssertionException);
    ASSERT_THROWS(ExpressionObject::parse(expCtx, BSON(std::string("a\0b", 3) << 1), vps),
                  AssertionException);
}

TEST(ExpressionObjectParse, ShouldRejectInvalidFieldPathAsValue) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    ASSERT_THROWS(ExpressionObject::parse(expCtx,
                                          BSON("a"
                                               << "$field."),
                                          vps),
                  AssertionException);
}

TEST(ParseObject, ShouldRejectExpressionAsTheSecondField) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    ASSERT_THROWS(
        ExpressionObject::parse(
            expCtx, BSON("a" << BSON("$and" << BSONArray()) << "$or" << BSONArray()), vps),
        AssertionException);
}

//
// Evaluation.
//

TEST(ExpressionObjectEvaluate, EmptyObjectShouldEvaluateToEmptyDocument) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object = ExpressionObject::create(expCtx, {});
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document()));
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document{{"a", 1}}));
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document{{"_id", "ID"_sd}}));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateEachField) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object =
        ExpressionObject::create(expCtx, {{"a", makeConstant(1)}, {"b", makeConstant(5)}});
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}), object->evaluate(Document()));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}), object->evaluate(Document{{"a", 1}}));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"_id", "ID"_sd}}));
}

TEST(ExpressionObjectEvaluate, OrderOfFieldsInOutputShouldMatchOrderInSpecification) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object = ExpressionObject::create(expCtx,
                                           {{"a", ExpressionFieldPath::create(expCtx, "a")},
                                            {"b", ExpressionFieldPath::create(expCtx, "b")},
                                            {"c", ExpressionFieldPath::create(expCtx, "c")}});
    ASSERT_VALUE_EQ(
        Value(Document{{"a", "A"_sd}, {"b", "B"_sd}, {"c", "C"_sd}}),
        object->evaluate(Document{{"c", "C"_sd}, {"a", "A"_sd}, {"b", "B"_sd}, {"_id", "ID"_sd}}));
}

TEST(ExpressionObjectEvaluate, ShouldRemoveFieldsThatHaveMissingValues) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object = ExpressionObject::create(expCtx,
                                           {{"a", ExpressionFieldPath::create(expCtx, "a.b")},
                                            {"b", ExpressionFieldPath::create(expCtx, "missing")}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document()));
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document{{"a", 1}}));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateFieldsWithinNestedObject) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object = ExpressionObject::create(
        expCtx,
        {{"a",
          ExpressionObject::create(
              expCtx,
              {{"b", makeConstant(1)}, {"c", ExpressionFieldPath::create(expCtx, "_id")}})}});
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}}}}), object->evaluate(Document()));
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}, {"c", "ID"_sd}}}}),
                    object->evaluate(Document{{"_id", "ID"_sd}}));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateToEmptyDocumentIfAllFieldsAreMissing) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object =
        ExpressionObject::create(expCtx, {{"a", ExpressionFieldPath::create(expCtx, "missing")}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document()));

    auto objectWithNestedObject = ExpressionObject::create(expCtx, {{"nested", object}});
    ASSERT_VALUE_EQ(Value(Document{{"nested", Document{}}}),
                    objectWithNestedObject->evaluate(Document()));
}

//
// Dependencies.
//

TEST(ExpressionObjectDependencies, ConstantValuesShouldNotBeAddedToDependencies) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object = ExpressionObject::create(expCtx, {{"a", makeConstant(5)}});
    DepsTracker deps;
    object->addDependencies(&deps);
    ASSERT_EQ(deps.fields.size(), 0UL);
}

TEST(ExpressionObjectDependencies, FieldPathsShouldBeAddedToDependencies) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto object =
        ExpressionObject::create(expCtx, {{"x", ExpressionFieldPath::create(expCtx, "c.d")}});
    DepsTracker deps;
    object->addDependencies(&deps);
    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("c.d"), 1UL);
};

TEST(ExpressionObjectDependencies, VariablesShouldBeAddedToDependencies) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto varID = expCtx->variablesParseState.defineVariable("var1");
    auto fieldPath = ExpressionFieldPath::parse(expCtx, "$$var1", expCtx->variablesParseState);
    DepsTracker deps;
    fieldPath->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(deps.vars.count(varID), 1UL);
}

TEST(ExpressionObjectDependencies, LocalLetVariablesShouldBeFilteredOutOfDependencies) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->variablesParseState.defineVariable("var1");
    auto letSpec = BSON("$let" << BSON("vars" << BSON("var2"
                                                      << "abc")
                                              << "in"
                                              << BSON("$multiply" << BSON_ARRAY("$$var1"
                                                                                << "$$var2"))));
    auto expressionLet =
        ExpressionLet::parse(expCtx, letSpec.firstElement(), expCtx->variablesParseState);
    DepsTracker deps;
    expressionLet->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(expCtx->variablesParseState.getVariable("var1"), *deps.vars.begin());
}

TEST(ExpressionObjectDependencies, LocalMapVariablesShouldBeFilteredOutOfDependencies) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->variablesParseState.defineVariable("var1");
    auto mapSpec = BSON("$map" << BSON("input"
                                       << "$field1"
                                       << "as"
                                       << "var2"
                                       << "in"
                                       << BSON("$multiply" << BSON_ARRAY("$$var1"
                                                                         << "$$var2"))));

    auto expressionMap =
        ExpressionMap::parse(expCtx, mapSpec.firstElement(), expCtx->variablesParseState);
    DepsTracker deps;
    expressionMap->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(expCtx->variablesParseState.getVariable("var1"), *deps.vars.begin());
}

TEST(ExpressionObjectDependencies, LocalFilterVariablesShouldBeFilteredOutOfDependencies) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->variablesParseState.defineVariable("var1");
    auto filterSpec = BSON("$filter" << BSON("input" << BSON_ARRAY(1 << 2 << 3) << "as"
                                                     << "var2"
                                                     << "cond"
                                                     << BSON("$gt" << BSON_ARRAY("$$var1"
                                                                                 << "$$var2"))));

    auto expressionFilter =
        ExpressionFilter::parse(expCtx, filterSpec.firstElement(), expCtx->variablesParseState);
    DepsTracker deps;
    expressionFilter->addDependencies(&deps);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(expCtx->variablesParseState.getVariable("var1"), *deps.vars.begin());
}

//
// Optimizations.
//

TEST(ExpressionObjectOptimizations, OptimizingAnObjectShouldOptimizeSubExpressions) {
    // Build up the object {a: {$add: [1, 2]}}.
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;
    auto addExpression =
        ExpressionAdd::parse(expCtx, BSON("$add" << BSON_ARRAY(1 << 2)).firstElement(), vps);
    auto object = ExpressionObject::create(expCtx, {{"a", addExpression}});
    ASSERT_EQ(object->getChildExpressions().size(), 1UL);

    auto optimized = object->optimize();
    auto optimizedObject = dynamic_cast<ExpressionObject*>(optimized.get());
    ASSERT_TRUE(optimizedObject);
    ASSERT_EQ(optimizedObject->getChildExpressions().size(), 1UL);

    // We should have optimized {$add: [1, 2]} to just the constant 3.
    auto expConstant =
        dynamic_cast<ExpressionConstant*>(optimizedObject->getChildExpressions()[0].second.get());
    ASSERT_TRUE(expConstant);
    ASSERT_VALUE_EQ(expConstant->evaluate(Document()), Value(3));
};

}  // namespace Object

namespace Or {

class ExpectedResultBase {
public:
    virtual ~ExpectedResultBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(expression->evaluate(fromBson(BSON("a" << 1)))));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()),
                          toBson(optimized->evaluate(fromBson(BSON("a" << 1)))));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual bool expectedResult() = 0;
};

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx->variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(expectedOptimized(), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class NoOptimizeBase : public OptimizeBase {
    BSONObj expectedOptimized() {
        return constify(spec());
    }
};

/** $or without operands. */
class NoOperands : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSONArray());
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'true'. */
class True : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'false'. */
class False : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'true', 'true'. */
class TrueTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(true << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'true', 'false'. */
class TrueFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(true << false));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'false', 'true'. */
class FalseTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed 'false', 'false'. */
class FalseFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'false', 'false', 'false'. */
class FalseFalseFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << false << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed 'false', 'false', 'true'. */
class FalseFalseTrue : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(false << false << true));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed '0', '1'. */
class ZeroOne : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << 1));
    }
    bool expectedResult() {
        return true;
    }
};

/** $or passed '0', 'false'. */
class ZeroFalse : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << false));
    }
    bool expectedResult() {
        return false;
    }
};

/** $or passed a field path. */
class FieldPath : public ExpectedResultBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
    bool expectedResult() {
        return true;
    }
};

/** A constant expression is optimized to a constant. */
class OptimizeConstantExpression : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** A non constant expression is not optimized. */
class NonConstant : public NoOptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantTrue : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantFalse : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
    // note: using $and as serialization of ExpressionCoerceToBool rather than
    // ExpressionAnd
};

/** An expression with a field path and '1'. */
class NonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a" << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression with a field path and '0'. */
class NonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a" << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression with two field paths and '1'. */
class NonConstantNonConstantOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"
                                        << 1));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression with two field paths and '0'. */
class NonConstantNonConstantZero : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"
                                        << 0));
    }
    BSONObj expectedOptimized() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"));
    }
};

/** An expression with '0', '1', and a field path. */
class ZeroOneNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << 1 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

/** An expression with '0', '0', and a field path. */
class ZeroZeroNonConstant : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << 0 << "$a"));
    }
    BSONObj expectedOptimized() {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** Nested $or expressions. */
class Nested : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << BSON("$or" << BSON_ARRAY(0)) << "$a"
                                          << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$or" << BSON_ARRAY("$a"
                                        << "$b"));
    }
};

/** Nested $or expressions containing a nested value evaluating to false. */
class NestedOne : public OptimizeBase {
    BSONObj spec() {
        return BSON("$or" << BSON_ARRAY(0 << BSON("$or" << BSON_ARRAY(BSON("$or" << BSON_ARRAY(1))))
                                          << "$a"
                                          << "$b"));
    }
    BSONObj expectedOptimized() {
        return BSON("$const" << true);
    }
};

}  // namespace Or

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
                Value result = expr->evaluate(Document());
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
                    {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(expCtx, obj, vps);
                        expr->evaluate(Document());
                    },
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
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class Redundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class DoubleRedundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(DOC_ARRAY(1 << 1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2)
                                          << "$setUnion"
                                          << DOC_ARRAY(1 << 2)
                                          << "$setDifference"
                                          << vector<Value>()));
    }
};

class Super : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(2)));
    }
};

class SuperWithRedundant : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 2) << DOC_ARRAY(1)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(2)));
    }
};

class Sub : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class SameBackwards : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(2 << 1)) << "expected"
                           << DOC("$setIsSubset" << true << "$setEquals" << true
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2)
                                                 << "$setDifference"
                                                 << vector<Value>()));
    }
};

class NoOverlap : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 4)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << vector<Value>()
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2 << 4 << 8)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(1 << 2)));
    }
};

class Overlap : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 2 << 4)) << "expected"
                           << DOC("$setIsSubset" << false << "$setEquals" << false
                                                 << "$setIntersection"
                                                 << DOC_ARRAY(2)
                                                 << "$setUnion"
                                                 << DOC_ARRAY(1 << 2 << 4 << 8)
                                                 << "$setDifference"
                                                 << DOC_ARRAY(1)));
    }
};

class LastNull : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << Value(BSONNULL)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference"
                                                     << BSONNULL)
                           << "error"
                           << DOC_ARRAY("$setEquals"_sd
                                        << "$setIsSubset"_sd));
    }
};

class FirstNull : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(Value(BSONNULL) << DOC_ARRAY(1 << 2)) << "expected"
                           << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                     << "$setDifference"
                                                     << BSONNULL)
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
                                                     << DOC_ARRAY(1 << 2)
                                                     << "$setIsSubset"
                                                     << true
                                                     << "$setEquals"
                                                     << false
                                                     << "$setDifference"
                                                     << vector<Value>()));
    }
};

class RightArgEmpty : public ExpectedResultBase {
    Document getSpec() {
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << vector<Value>()) << "expected"
                           << DOC("$setIntersection" << vector<Value>() << "$setUnion"
                                                     << DOC_ARRAY(1 << 2)
                                                     << "$setIsSubset"
                                                     << false
                                                     << "$setEquals"
                                                     << false
                                                     << "$setDifference"
                                                     << DOC_ARRAY(1 << 2)));
    }
};

class ManyArgs : public ExpectedResultBase {
    Document getSpec() {
        return DOC(
            "input" << DOC_ARRAY(DOC_ARRAY(8 << 3) << DOC_ARRAY("asdf"_sd
                                                                << "foo"_sd)
                                                   << DOC_ARRAY(80.3 << 34)
                                                   << vector<Value>()
                                                   << DOC_ARRAY(80.3 << "foo"_sd << 11 << "yay"_sd))
                    << "expected"
                    << DOC("$setIntersection" << vector<Value>() << "$setEquals" << false
                                              << "$setUnion"
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
        return DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 4) << DOC_ARRAY(1 << 2 << 2 << 4)
                                                               << DOC_ARRAY(4 << 1 << 2)
                                                               << DOC_ARRAY(2 << 1 << 1 << 4))
                           << "expected"
                           << DOC("$setIntersection" << DOC_ARRAY(1 << 2 << 4) << "$setEquals"
                                                     << true
                                                     << "$setUnion"
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
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult), toBson(expression->evaluate(Document())));
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
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()), toBson(expression->evaluate(Document())));
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

}  // namespace Substr

namespace SubstrCP {

TEST(ExpressionSubstrCPTest, DoesThrowWithBadContinuationByte) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    const auto continuationByte = "\x80\x00"_sd;
    const auto expr = Expression::parseExpression(
        expCtx, BSON("$substrCP" << BSON_ARRAY(continuationByte << 0 << 1)), vps);
    ASSERT_THROWS({ expr->evaluate(Document()); }, AssertionException);
}

TEST(ExpressionSubstrCPTest, DoesThrowWithInvalidLeadingByte) {
    intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    VariablesParseState vps = expCtx->variablesParseState;

    const auto leadingByte = "\xFF\x00"_sd;
    const auto expr = Expression::parseExpression(
        expCtx, BSON("$substrCP" << BSON_ARRAY(leadingByte << 0 << 1)), vps);
    ASSERT_THROWS({ expr->evaluate(Document()); }, AssertionException);
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
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()), toBson(expression->evaluate(Document())));
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
        ASSERT_BSONOBJ_EQ(BSON("" << expectedResult()), toBson(expression->evaluate(Document())));
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
                const Value result = expr->evaluate(Document());
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
                    {
                        // NOTE: parse and evaluatation failures are treated the
                        // same
                        const intrusive_ptr<Expression> expr =
                            Expression::parseExpression(expCtx, obj, vps);
                        expr->evaluate(Document());
                    },
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
        return DOC("input" << DOC_ARRAY(BSONNULL) << "error" << DOC_ARRAY("$allElementsTrue"_sd
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

namespace ExpressionDateFromPartsTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateFromPartsTest = AggregationContextFixture;

TEST_F(ExpressionDateFromPartsTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec =
        BSON("$dateFromParts" << BSON(
                 "year" << 2017 << "month" << 6 << "day" << 27 << "hour" << 14 << "minute" << 37
                        << "second"
                        << 15
                        << "millisecond"
                        << 414
                        << "timezone"
                        << "America/Los_Angeles"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateFromParts",
                        Document{{"year", Document{{"$const", 2017}}},
                                 {"month", Document{{"$const", 6}}},
                                 {"day", Document{{"$const", 27}}},
                                 {"hour", Document{{"$const", 14}}},
                                 {"minute", Document{{"$const", 37}}},
                                 {"second", Document{{"$const", 15}}},
                                 {"millisecond", Document{{"$const", 414}}},
                                 {"timezone", Document{{"$const", "America/Los_Angeles"_sd}}}}}});
    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateFromPartsTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$dateFromParts" << BSON("year" << 2017));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year, month and day are provided, and are both
    // constants.
    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "month" << 6 << "day" << 27));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year, hour and minute are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateFromParts" << BSON("year" << BSON("$add" << BSON_ARRAY(1900 << 107)) << "hour"
                                                << BSON("$add" << BSON_ARRAY(13 << 1))
                                                << "minute"
                                                << BSON("$add" << BSON_ARRAY(40 << 3))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year and milliseconds are provided, and year is an
    // expressions which evaluate to a constant, with milliseconds a constant
    spec = BSON("$dateFromParts" << BSON(
                    "year" << BSON("$add" << BSON_ARRAY(1900 << 107)) << "millisecond" << 514));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both isoWeekYear, and isoWeek are provided, and are both
    // constants.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << 2017 << "isoWeek" << 26));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both isoWeekYear, isoWeek and isoDayOfWeek are provided,
    // and are both expressions which evaluate to constants.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << BSON("$add" << BSON_ARRAY(1017 << 1000))
                                                       << "isoWeek"
                                                       << BSON("$add" << BSON_ARRAY(20 << 6))
                                                       << "isoDayOfWeek"
                                                       << BSON("$add" << BSON_ARRAY(3 << 2))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both year and month are provided, but
    // year is not a constant.
    spec = BSON("$dateFromParts" << BSON("year"
                                         << "$year"
                                         << "month"
                                         << 6));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both year and day are provided, but
    // day is not a constant.
    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "day"
                                                << "$day"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both isoWeekYear and isoDayOfWeek are provided,
    // but
    // isoDayOfWeek is not a constant.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << 2017 << "isoDayOfWeek"
                                                       << "$isoDayOfWeekday"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

}  // namespace ExpressionDateFromPartsTest

namespace ExpressionDateToPartsTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateToPartsTest = AggregationContextFixture;

TEST_F(ExpressionDateToPartsTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                                      << "Europe/London"
                                                      << "iso8601"
                                                      << false));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateToParts",
                        Document{{"date", Document{{"$const", Date_t{}}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"iso8601", Document{{"$const", false}}}}}});
    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateToPartsTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$dateToParts" << BSON("date" << Date_t{}));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and timezone are provided, and are both
    // constants.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                              << "UTC"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and timezone are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToParts" << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                              << "timezone"
                                              << BSON("$concat" << BSON_ARRAY("Europe"
                                                                              << "/"
                                                                              << "London"))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and iso8601 are provided, and are both
    // constants.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "iso8601" << true));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and iso8601 are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToParts" << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                              << "iso8601"
                                              << BSON("$not" << false)));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and timezone are provided, but
    // date is not a constant.
    spec = BSON("$dateToParts" << BSON("date"
                                       << "$date"
                                       << "timezone"
                                       << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and timezone are provided, but
    // timezone is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                              << "$tz"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and iso8601 are provided, but
    // iso8601 is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "iso8601"
                                              << "$iso8601"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

}  // namespace ExpressionDateToPartsTest

namespace DateExpressionsTest {

std::vector<StringData> dateExpressions = {"$year"_sd,
                                           "$isoWeekYear"_sd,
                                           "$month"_sd,
                                           "$dayOfMonth"_sd,
                                           "$hour"_sd,
                                           "$minute"_sd,
                                           "$second"_sd,
                                           "$millisecond"_sd,
                                           "$week"_sd,
                                           "$isoWeek"_sd,
                                           "$dayOfYear"_sd};

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using DateExpressionTest = AggregationContextFixture;

TEST_F(DateExpressionTest, ParsingAcceptsAllFormats) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto possibleSyntaxes = {
            // Single argument.
            BSON(expName << Date_t{}),
            BSON(expName << "$date"),
            BSON(expName << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))),
            // Single argument wrapped in an array.
            BSON(expName << BSON_ARRAY("$date")),
            BSON(expName << BSON_ARRAY(Date_t{})),
            BSON(expName << BSON_ARRAY(BSON("$add" << BSON_ARRAY(Date_t{} << 1000)))),
            // Object literal syntax.
            BSON(expName << BSON("date" << Date_t{})),
            BSON(expName << BSON("date"
                                 << "$date")),
            BSON(expName << BSON("date" << BSON("$add" << BSON_ARRAY("$date" << 1000)))),
            BSON(expName << BSON("date" << Date_t{} << "timezone"
                                        << "Europe/London")),
            BSON(expName << BSON("date" << Date_t{} << "timezone"
                                        << "$tz"))};
        for (auto&& syntax : possibleSyntaxes) {
            Expression::parseExpression(expCtx, syntax, expCtx->variablesParseState);
        }
    }
}

TEST_F(DateExpressionTest, ParsingRejectsUnrecognizedFieldsInObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "Europe/London"
                                                   << "extra"
                                                   << 4));
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40535);
    }
}

TEST_F(DateExpressionTest, ParsingRejectsEmptyObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSONObj());
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40539);
    }
}

TEST_F(DateExpressionTest, RejectsEmptyArray) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSONArray());
        // It will parse as an ExpressionArray, and fail at runtime.
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40536);
    }
}

TEST_F(DateExpressionTest, RejectsArraysWithMoreThanOneElement) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON_ARRAY("$date"
                                                  << "$tz"));
        // It will parse as an ExpressionArray, and fail at runtime.
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40536);
    }
}

TEST_F(DateExpressionTest, RejectsArraysWithinObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << BSON_ARRAY(Date_t{}) << "timezone"
                                                   << "Europe/London"));
        // It will parse as an ExpressionArray, and fail at runtime.
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(dateExp->evaluate(contextDoc), AssertionException, 16006);

        // Test that it rejects an array for the timezone option.
        spec =
            BSON(expName << BSON("date" << Date_t{} << "timezone" << BSON_ARRAY("Europe/London")));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(dateExp->evaluate(contextDoc), AssertionException, 40533);
    }
}

TEST_F(DateExpressionTest, RejectsTypesThatCannotCoerceToDate) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$stringField");
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"stringField", "string"_sd}};
        ASSERT_THROWS_CODE(dateExp->evaluate(contextDoc), AssertionException, 16006);
    }
}

TEST_F(DateExpressionTest, AcceptsObjectIds) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$oid");
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"oid", OID::gen()}};
        dateExp->evaluate(contextDoc);  // Should not throw.
    }
}

TEST_F(DateExpressionTest, AcceptsTimestamps) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$ts");
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"ts", Timestamp{Date_t{}}}};
        dateExp->evaluate(contextDoc);  // Should not throw.
    }
}

TEST_F(DateExpressionTest, RejectsNonStringTimezone) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "$intField"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"intField", 4}};
        ASSERT_THROWS_CODE(dateExp->evaluate(contextDoc), AssertionException, 40533);
    }
}

TEST_F(DateExpressionTest, RejectsUnrecognizedTimeZoneSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "UNRECOGNIZED!"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(dateExp->evaluate(contextDoc), AssertionException, 40485);
    }
}

TEST_F(DateExpressionTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        // Test that it serializes to the full format if given an object specification.
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "Europe/London"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto expectedSerialization =
            Value(Document{{expName,
                            Document{{"date", Document{{"$const", Date_t{}}}},
                                     {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});
        ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

        // Test that it serializes to the full format if given a date.
        spec = BSON(expName << Date_t{});
        expectedSerialization =
            Value(Document{{expName, Document{{"date", Document{{"$const", Date_t{}}}}}}});
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

        // Test that it serializes to the full format if given a date within an array.
        spec = BSON(expName << BSON_ARRAY(Date_t{}));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
    }
}

TEST_F(DateExpressionTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        // Test that it becomes a constant if only date is provided, and it is constant.
        auto spec = BSON(expName << BSON("date" << Date_t{}));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it becomes a constant if both date and timezone are provided, and are both
        // constants.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it becomes a constant if both date and timezone are provided, and are both
        // expressions which evaluate to constants.
        spec = BSON(expName << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                           << "timezone"
                                           << BSON("$concat" << BSON_ARRAY("Europe"
                                                                           << "/"
                                                                           << "London"))));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it does *not* become a constant if both date and timezone are provided, but
        // date is not a constant.
        spec = BSON(expName << BSON("date"
                                    << "$date"
                                    << "timezone"
                                    << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it does *not* become a constant if both date and timezone are provided, but
        // timezone is not a constant.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "$tz"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
    }
}

TEST_F(DateExpressionTest, DoesRespectTimeZone) {
    // Make sure they each successfully evaluate with a different TimeZone.
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                << "America/New_York"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        dateExp->evaluate(contextDoc);  // Should not throw.
    }

    // Make sure the time zone is used during evaluation.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);  // 2017-06-06T19:38:43:234Z.
    auto specWithoutTimezone = BSON("$hour" << BSON("date" << date));
    auto hourWithoutTimezone =
        Expression::parseExpression(expCtx, specWithoutTimezone, expCtx->variablesParseState)
            ->evaluate({});
    ASSERT_VALUE_EQ(hourWithoutTimezone, Value(19));

    auto specWithTimezone = BSON("$hour" << BSON("date" << date << "timezone"
                                                        << "America/New_York"));
    auto hourWithTimezone =
        Expression::parseExpression(expCtx, specWithTimezone, expCtx->variablesParseState)
            ->evaluate({});
    ASSERT_VALUE_EQ(hourWithTimezone, Value(15));
}

TEST_F(DateExpressionTest, DoesResultInNullIfGivenNullishInput) {
    // Make sure they each successfully evaluate with a different TimeZone.
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto contextDoc = Document{{"_id", 0}};

        // Test that the expression results in null if the date is nullish and the timezone is not
        // specified.
        auto spec = BSON(expName << BSON("date"
                                         << "$missing"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        spec = BSON(expName << BSON("date" << BSONNULL));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        spec = BSON(expName << BSON("date" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        // Test that the expression results in null if the date is present but the timezone is
        // nullish.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "$missing"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        spec = BSON(expName << BSON("date" << Date_t{} << "timezone" << BSONNULL));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        spec = BSON(expName << BSON("date" << Date_t{} << "timezone" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        // Test that the expression results in null if the date and timezone both nullish.
        spec = BSON(expName << BSON("date"
                                    << "$missing"
                                    << "timezone"
                                    << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));

        // Test that the expression results in null if the date is nullish and timezone is present.
        spec = BSON(expName << BSON("date"
                                    << "$missing"
                                    << "timezone"
                                    << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc));
    }
}

}  // namespace DateExpressionsTest

namespace ExpressionDateToStringTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateToStringTest = AggregationContextFixture;

TEST_F(ExpressionDateToStringTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateToString" << BSON("date" << Date_t{} << "timezone"
                                                       << "Europe/London"
                                                       << "format"
                                                       << "%Y-%m-%d"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateToString",
                        Document{{"format", "%Y-%m-%d"_sd},
                                 {"date", Document{{"$const", Date_t{}}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateToStringTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();

    // Test that it becomes a constant if both format and date are constant, and timezone is
    // missing.
    auto spec = BSON("$dateToString" << BSON("format"
                                             << "%Y-%m-%d"
                                             << "date"
                                             << Date_t{}));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both format, date and timezone are provided, and are both
    // constants.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}
                                        << "timezone"
                                        << "Europe/Amsterdam"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both format, date and timezone are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m%d"
                                        << "date"
                                        << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                        << "timezone"
                                        << BSON("$concat" << BSON_ARRAY("Europe"
                                                                        << "/"
                                                                        << "London"))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both format, date and timezone are provided, but
    // date is not a constant.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << "$date"
                                        << "timezone"
                                        << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both format, date and timezone are provided, but
    // timezone is not a constant.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}
                                        << "timezone"
                                        << "$tz"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}
}  // namespace ExpressionDateToStringTest

namespace ExpressionDateFromStringTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateFromStringTest = AggregationContextFixture;

TEST_F(ExpressionDateFromStringTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateFromString" << BSON("dateString"
                                                  << "2017-07-04T13:06:44Z"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization = Value(
        Document{{"$dateFromString",
                  Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

    // Test that it serializes to the full format if given an object specification.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:06:44Z"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateFromStringTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    // Test that it becomes a constant with just the dateString.
    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-04T13:09:57Z"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    Date_t dateVal = Date_t::fromMillisSinceEpoch(1499173797000);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));

    // Test that it becomes a constant with the dateString and timezone being a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    dateVal = Date_t::fromMillisSinceEpoch(1499170197000);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));

    // Test that it does *not* become a constant if dateString is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "$date"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if timezone is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57Z"
                                          << "timezone"
                                          << "$tz"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

TEST_F(ExpressionDateFromStringTest, RejectsUnparsableString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "60.Monday1770/06:59"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40553);
}

TEST_F(ExpressionDateFromStringTest, RejectsTimeZoneInString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-13T10:02:57 Europe/London"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40553);

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "July 4, 2017 Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40553);
}

TEST_F(ExpressionDateFromStringTest, RejectsTimeZoneInStringAndArgument) {
    auto expCtx = getExpCtx();

    // Test with "Z" and timezone
    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-14T15:24:38Z"
                                               << "timezone"
                                               << "Europe/London"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40551);

    // Test with timezone abbreviation and timezone
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-14T15:24:38 PDT"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40551);

    // Test with GMT offset and timezone
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-14T15:24:38+02:00"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40554);

    // Test with GMT offset and GMT timezone
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-14 -0400"
                                          << "timezone"
                                          << "GMT"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}), AssertionException, 40554);
}

TEST_F(ExpressionDateFromStringTest, ReadWithUTCOffset) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-28T10:47:52.912"
                                               << "timezone"
                                               << "-01:00"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto dateVal = Date_t::fromMillisSinceEpoch(1501242472912);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-28T10:47:52.912"
                                          << "timezone"
                                          << "+01:00"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1501235272912);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-28T10:47:52.912"
                                          << "timezone"
                                          << "+0445"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1501221772912);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-28T10:47:52.912"
                                          << "timezone"
                                          << "+10:45"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1501200172912);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "1945-07-28T10:47:52.912"
                                          << "timezone"
                                          << "-08:00"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(-770879527088);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate(Document{}));
}

}  // namespace ExpressionDateFromStringTest

class All : public Suite {
public:
    All() : Suite("expression") {}
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

        add<And::NoOperands>();
        add<And::True>();
        add<And::False>();
        add<And::TrueTrue>();
        add<And::TrueFalse>();
        add<And::FalseTrue>();
        add<And::FalseFalse>();
        add<And::TrueTrueTrue>();
        add<And::TrueTrueFalse>();
        add<And::TrueTrueFalse>();
        add<And::ZeroOne>();
        add<And::OneTwo>();
        add<And::FieldPath>();
        add<And::OptimizeConstantExpression>();
        add<And::NonConstant>();
        add<And::ConstantNonConstantTrue>();
        add<And::ConstantNonConstantFalse>();
        add<And::NonConstantOne>();
        add<And::NonConstantZero>();
        add<And::NonConstantNonConstantOne>();
        add<And::NonConstantNonConstantZero>();
        add<And::ZeroOneNonConstant>();
        add<And::OneOneNonConstant>();
        add<And::Nested>();
        add<And::NestedZero>();

        add<CoerceToBool::EvaluateTrue>();
        add<CoerceToBool::EvaluateFalse>();
        add<CoerceToBool::Dependencies>();
        add<CoerceToBool::AddToBsonObj>();
        add<CoerceToBool::AddToBsonArray>();

        add<Compare::EqLt>();
        add<Compare::EqEq>();
        add<Compare::EqGt>();
        add<Compare::NeLt>();
        add<Compare::NeEq>();
        add<Compare::NeGt>();
        add<Compare::GtLt>();
        add<Compare::GtEq>();
        add<Compare::GtGt>();
        add<Compare::GteLt>();
        add<Compare::GteEq>();
        add<Compare::GteGt>();
        add<Compare::LtLt>();
        add<Compare::LtEq>();
        add<Compare::LtGt>();
        add<Compare::LteLt>();
        add<Compare::LteEq>();
        add<Compare::LteGt>();
        add<Compare::CmpLt>();
        add<Compare::CmpEq>();
        add<Compare::CmpGt>();
        add<Compare::CmpBracketed>();
        add<Compare::ZeroOperands>();
        add<Compare::OneOperand>();
        add<Compare::ThreeOperands>();
        add<Compare::IncompatibleTypes>();
        add<Compare::OptimizeConstants>();
        add<Compare::NoOptimizeCmp>();
        add<Compare::NoOptimizeNe>();
        add<Compare::NoOptimizeNoConstant>();
        add<Compare::NoOptimizeWithoutFieldPath>();
        add<Compare::NoOptimizeWithoutFieldPathReverse>();
        add<Compare::OptimizeEq>();
        add<Compare::OptimizeEqReverse>();
        add<Compare::OptimizeLt>();
        add<Compare::OptimizeLtReverse>();
        add<Compare::OptimizeLte>();
        add<Compare::OptimizeLteReverse>();
        add<Compare::OptimizeGt>();
        add<Compare::OptimizeGtReverse>();
        add<Compare::OptimizeGte>();
        add<Compare::OptimizeGteReverse>();

        add<Constant::Create>();
        add<Constant::CreateFromBsonElement>();
        add<Constant::Optimize>();
        add<Constant::Dependencies>();
        add<Constant::AddToBsonObj>();
        add<Constant::AddToBsonArray>();

        add<FieldPath::Invalid>();
        add<FieldPath::Dependencies>();
        add<FieldPath::Missing>();
        add<FieldPath::Present>();
        add<FieldPath::NestedBelowNull>();
        add<FieldPath::NestedBelowUndefined>();
        add<FieldPath::NestedBelowMissing>();
        add<FieldPath::NestedBelowInt>();
        add<FieldPath::NestedValue>();
        add<FieldPath::NestedBelowEmptyObject>();
        add<FieldPath::NestedBelowEmptyArray>();
        add<FieldPath::NestedBelowEmptyArray>();
        add<FieldPath::NestedBelowArrayWithNull>();
        add<FieldPath::NestedBelowArrayWithUndefined>();
        add<FieldPath::NestedBelowArrayWithInt>();
        add<FieldPath::NestedWithinArray>();
        add<FieldPath::MultipleArrayValues>();
        add<FieldPath::ExpandNestedArrays>();
        add<FieldPath::AddToBsonObj>();
        add<FieldPath::AddToBsonArray>();


        add<Or::NoOperands>();
        add<Or::True>();
        add<Or::False>();
        add<Or::TrueTrue>();
        add<Or::TrueFalse>();
        add<Or::FalseTrue>();
        add<Or::FalseFalse>();
        add<Or::FalseFalseFalse>();
        add<Or::FalseFalseTrue>();
        add<Or::ZeroOne>();
        add<Or::ZeroFalse>();
        add<Or::FieldPath>();
        add<Or::OptimizeConstantExpression>();
        add<Or::NonConstant>();
        add<Or::ConstantNonConstantTrue>();
        add<Or::ConstantNonConstantFalse>();
        add<Or::NonConstantOne>();
        add<Or::NonConstantZero>();
        add<Or::NonConstantNonConstantOne>();
        add<Or::NonConstantNonConstantZero>();
        add<Or::ZeroOneNonConstant>();
        add<Or::ZeroZeroNonConstant>();
        add<Or::Nested>();
        add<Or::NestedOne>();

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

SuiteInstance<All> myall;

}  // namespace ExpressionTests

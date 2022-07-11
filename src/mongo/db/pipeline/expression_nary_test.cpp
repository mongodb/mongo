/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/config.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace ExpressionTests {
using boost::intrusive_ptr;
using std::numeric_limits;
using std::set;
using std::string;
using std::vector;

/** A dummy child of ExpressionNary used for testing. */
class Testable : public ExpressionNary {
public:
    virtual Value evaluate(const Document& root, Variables* variables) const {
        // Just put all the values in a list.
        // By default, this is not associative/commutative so the results will change if
        // instantiated as commutative or associative and operations are reordered.
        vector<Value> values;
        for (auto&& child : _children)
            values.push_back(child->evaluate(root, variables));
        return Value(values);
    }

    virtual const char* getOpName() const {
        return "$testable";
    }

    virtual Associativity getAssociativity() const {
        return _associativity;
    }

    virtual bool isCommutative() const {
        return _isCommutative;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    static intrusive_ptr<Testable> create(ExpressionContext* const expCtx,
                                          Associativity associative,
                                          bool commutative) {
        return new Testable(expCtx, associative, commutative);
    }

private:
    Testable(ExpressionContext* const expCtx, Associativity associativity, bool isCommutative)
        : ExpressionNary(expCtx), _associativity(associativity), _isCommutative(isCommutative) {}
    Associativity _associativity;
    bool _isCommutative;
};

namespace {

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

/** Convert Expression to BSON. */
static BSONObj expressionToBson(const intrusive_ptr<Expression>& expression) {
    return BSON("" << expression->serialize(false)).firstElement().embeddedObject().getOwned();
}

class ExpressionBaseTest : public unittest::Test {
public:
    void addOperand(intrusive_ptr<ExpressionNary> expr, Value arg) {
        expr->addOperand(ExpressionConstant::create(&expCtx, arg));
    }

protected:
    ExpressionContextForTest expCtx;
};

class ExpressionNaryTestOneArg : public ExpressionBaseTest {
public:
    Value eval(Value input) {
        addOperand(_expr, input);
        return _expr->evaluate({}, &_expr->getExpressionContext()->variables);
    }
    virtual void assertEvaluates(Value input, Value output) {
        Value v = eval(input);
        ASSERT_VALUE_EQ(output, v);
        ASSERT_EQUALS(output.getType(), v.getType());
    }

    intrusive_ptr<ExpressionNary> _expr;
};

class ExpressionNaryTestTwoArg : public ExpressionBaseTest {
public:
    virtual void assertEvaluates(Value input1, Value input2, Value output) {
        addOperand(_expr, input1);
        addOperand(_expr, input2);
        ASSERT_VALUE_EQ(output, _expr->evaluate({}, &_expr->getExpressionContext()->variables));
        ASSERT_EQUALS(output.getType(),
                      _expr->evaluate({}, &_expr->getExpressionContext()->variables).getType());
    }

    intrusive_ptr<ExpressionNary> _expr;
};

/* ------------------------- NaryExpression -------------------------- */

class ExpressionNaryTest : public unittest::Test {
public:
    virtual void setUp() override {
        _notAssociativeNorCommutative =
            Testable::create(&expCtx, ExpressionNary::Associativity::kNone, false);
        _associativeOnly = Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
        _associativeAndCommutative =
            Testable::create(&expCtx, ExpressionNary::Associativity::kFull, true);
        _leftAssociativeOnly =
            Testable::create(&expCtx, ExpressionNary::Associativity::kLeft, false);
    }

protected:
    void assertDependencies(const intrusive_ptr<Testable>& expr,
                            const BSONArray& expectedDependencies) {
        DepsTracker dependencies;
        expr->addDependencies(&dependencies);
        BSONArrayBuilder dependenciesBson;
        for (OrderedPathSet::const_iterator i = dependencies.fields.begin();
             i != dependencies.fields.end();
             ++i) {
            dependenciesBson << *i;
        }
        ASSERT_BSONOBJ_EQ(expectedDependencies, dependenciesBson.arr());
        ASSERT_EQUALS(false, dependencies.needWholeDocument);
        ASSERT_EQUALS(false, dependencies.getNeedsAnyMetadata());
    }

    void assertContents(const intrusive_ptr<Testable>& expr, const BSONArray& expectedContents) {
        ASSERT_BSONOBJ_EQ(constify(BSON("$testable" << expectedContents)), expressionToBson(expr));
    }

    void addOperandArrayToExpr(const intrusive_ptr<Testable>& expr, const BSONArray& operands) {
        VariablesParseState vps = expCtx.variablesParseState;
        BSONObjIterator i(operands);
        while (i.more()) {
            BSONElement element = i.next();
            expr->addOperand(Expression::parseOperand(&expCtx, element, vps));
        }
    }

    ExpressionContextForTest expCtx;
    intrusive_ptr<Testable> _notAssociativeNorCommutative;
    intrusive_ptr<Testable> _associativeOnly;
    intrusive_ptr<Testable> _associativeAndCommutative;
    intrusive_ptr<Testable> _leftAssociativeOnly;
};

TEST_F(ExpressionNaryTest, AddedConstantOperandIsSerialized) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(9)));
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY(9));
}

TEST_F(ExpressionNaryTest, AddedFieldPathOperandIsSerialized) {
    _notAssociativeNorCommutative->addOperand(
        ExpressionFieldPath::deprecatedCreate(&expCtx, "ab.c"));
    assertContents(_notAssociativeNorCommutative, BSON_ARRAY("$ab.c"));
}

TEST_F(ExpressionNaryTest, ValidateEmptyDependencies) {
    assertDependencies(_notAssociativeNorCommutative, BSONArray());
}

TEST_F(ExpressionNaryTest, ValidateConstantExpressionDependency) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(1)));
    assertDependencies(_notAssociativeNorCommutative, BSONArray());
}

TEST_F(ExpressionNaryTest, ValidateFieldPathExpressionDependency) {
    _notAssociativeNorCommutative->addOperand(
        ExpressionFieldPath::deprecatedCreate(&expCtx, "ab.c"));
    assertDependencies(_notAssociativeNorCommutative, BSON_ARRAY("ab.c"));
}

TEST_F(ExpressionNaryTest, ValidateObjectExpressionDependency) {
    BSONObj spec = BSON("" << BSON("a"
                                   << "$x"
                                   << "q"
                                   << "$r"));
    BSONElement specElement = spec.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    _notAssociativeNorCommutative->addOperand(
        Expression::parseObject(&expCtx, specElement.Obj(), vps));
    assertDependencies(_notAssociativeNorCommutative,
                       BSON_ARRAY("r"
                                  << "x"));
}

TEST_F(ExpressionNaryTest, SerializationToBsonObj) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5)))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(false)));
}

TEST_F(ExpressionNaryTest, SerializationToBsonArr) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
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

TEST_F(ExpressionNaryTest, GroupingOptimizationOnLeftAssociativeOnlyFrontOperands) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(_leftAssociativeOnly, BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path"));
}


TEST_F(ExpressionNaryTest, GroupingOptimizationOnlyExecuteOnLeftAssociativeOnlyFrontConstants) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path" << 77 << 88 << "$path1" << 99 << 1010);
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(
        _leftAssociativeOnly,
        BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path" << 77 << 88 << "$path1" << 99 << 1010));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyMiddleOperands) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationNotExecuteOnLeftAssociativeOnlyMiddleOperands) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << 66 << "$path");
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(_leftAssociativeOnly, BSON_ARRAY("$path1" << 55 << 66 << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyBackOperands) {
    BSONArray spec = BSON_ARRAY("$path" << 55 << 66);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationNotExecuteOnLeftAssociativeOnlyBackOperands) {
    BSONArray spec = BSON_ARRAY("$path" << 55 << 66);
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(_leftAssociativeOnly, BSON_ARRAY("$path" << 55 << 66));
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kNone, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kNone, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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


// Test that if there is an expression of the same type as the first operand
// in a non-commutative but left-associative expression, the inner expression is not expanded.
TEST_F(ExpressionNaryTest,
       FlattenInnerOperandsOptimizationOnLeftAssociativeFrontOperandAndGroupIsNoOp) {
    BSONArrayBuilder specBuilder;

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kLeft, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY("$path" << 100 << 200 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _leftAssociativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_leftAssociativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_leftAssociativeOnly, spec);
    intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);

    assertContents(_leftAssociativeOnly, spec);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(200 << "$path3" << BSON_ARRAY(201 << 100) << "$path1"
                                               << BSON_ARRAY(101 << 99) << "$path2");
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    intrusive_ptr<Testable> innerOperand2 =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    intrusive_ptr<Testable> innerOperand2 =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
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

    intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, true);
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

/* ------------------------- ExpressionTrunc -------------------------- */

class ExpressionTruncOneArgTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionTrunc(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

class ExpressionTruncTwoArgTest : public ExpressionNaryTestTwoArg {
public:
    void assertEval(ImplicitValue input1, ImplicitValue input2, ImplicitValue output) {
        _expr = new ExpressionTrunc(&expCtx);
        ExpressionNaryTestTwoArg::assertEvaluates(input1, input2, output);
    }
};

TEST_F(ExpressionTruncOneArgTest, IntArg1) {
    assertEval(0, 0);
    assertEval(0, 0);
    assertEval(numeric_limits<int>::min(), numeric_limits<int>::min());
    assertEval(numeric_limits<int>::max(), numeric_limits<int>::max());
}

TEST_F(ExpressionTruncTwoArgTest, IntArg2) {
    assertEval(0, 0, 0);
    assertEval(2, -1, 0);
    assertEval(29, -1, 20);
    assertEval(numeric_limits<int>::min(), 10, numeric_limits<int>::min());
    assertEval(numeric_limits<int>::max(), 42, numeric_limits<int>::max());
}

TEST_F(ExpressionTruncOneArgTest, LongArg1) {
    assertEval(0LL, 0LL);
    assertEval(numeric_limits<long long>::min(), numeric_limits<long long>::min());
    assertEval(numeric_limits<long long>::max(), numeric_limits<long long>::max());
}

TEST_F(ExpressionTruncTwoArgTest, LongArg2) {
    assertEval(0LL, 0LL, 0LL);
    assertEval(2LL, -1LL, 0LL);
    assertEval(29LL, -1LL, 20LL);
    assertEval(numeric_limits<long long>::min(), 10LL, numeric_limits<long long>::min());
    assertEval(numeric_limits<long long>::max(), 42LL, numeric_limits<long long>::max());
}

TEST_F(ExpressionTruncOneArgTest, DoubleArg1) {
    assertEval(2.0, 2.0);
    assertEval(-2.0, -2.0);
    assertEval(0.9, 0.0);
    assertEval(0.1, 0.0);
    assertEval(0.5, 0.0);
    assertEval(1.5, 1.0);
    assertEval(2.5, 2.0);
    assertEval(-1.2, -1.0);
    assertEval(-1.7, -1.0);
    assertEval(-0.5, -0.0);
    assertEval(-1.5, -1.0);
    assertEval(-2.5, -2.0);

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // should just preserve the number).
    double largerThanLong = static_cast<double>(numeric_limits<long long>::max()) * 2.0;
    assertEval(largerThanLong, largerThanLong);
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEval(smallerThanLong, smallerThanLong);
}

TEST_F(ExpressionTruncTwoArgTest, DoubleArg2) {
    assertEval(2.0, 1.0, 2.0);
    assertEval(-2.0, 2.0, -2.0);
    assertEval(0.9, 0, 0.0);
    assertEval(0.1, 0, 0.0);
    assertEval(0.5, 0, 0.0);
    assertEval(1.5, 0, 1.0);
    assertEval(2.5, 0, 2.0);
    assertEval(-1.2, 0, -1.0);
    assertEval(-1.7, 0, -1.0);
    assertEval(-0.5, 0, -0.0);
    assertEval(-1.5, 0, -1.0);
    assertEval(-2.5, 0, -2.0);

    assertEval(-3.14159265, 0, -3.0);
    assertEval(-3.14159265, 1, -3.1);
    assertEval(-3.14159265, 2, -3.14);
    assertEval(-3.14159265, 3, -3.141);
    assertEval(-3.14159265, 4, -3.1415);
    assertEval(-3.14159265, 5, -3.14159);
    assertEval(-3.14159265, 6, -3.141592);
    assertEval(-3.14159265, 7, -3.1415926);
    assertEval(-3.14159265, 100, -3.14159265);
    assertEval(3.14159265, 0, 3.0);
    assertEval(3.14159265, 1, 3.1);
    assertEval(3.14159265, 2, 3.14);
    assertEval(3.14159265, 3, 3.141);
    assertEval(3.14159265, 4, 3.1415);
    assertEval(3.14159265, 5, 3.14159);
    assertEval(3.14159265, 6, 3.141592);
    assertEval(3.14159265, 7, 3.1415926);
    assertEval(3.14159265, 100, 3.14159265);
    assertEval(3.14159265, -1, 0.0);
    assertEval(335.14159265, -1, 330.0);
    assertEval(333.14159265, -2, 300.0);
}

TEST_F(ExpressionTruncOneArgTest, DecimalArg1) {
    assertEval(Decimal128("2"), Decimal128("2.0"));
    assertEval(Decimal128("-2"), Decimal128("-2.0"));
    assertEval(Decimal128("0.9"), Decimal128("0.0"));
    assertEval(Decimal128("0.1"), Decimal128("0.0"));
    assertEval(Decimal128("-1.2"), Decimal128("-1.0"));
    assertEval(Decimal128("-1.7"), Decimal128("-1.0"));
    assertEval(Decimal128("123456789.9999999999999999999999999"), Decimal128("123456789"));
    assertEval(Decimal128("-99999999999999999999999999999.99"),
               Decimal128("-99999999999999999999999999999.00"));
    assertEval(Decimal128("3.4E-6000"), Decimal128("0"));
}

TEST_F(ExpressionTruncTwoArgTest, DecimalArg2) {
    assertEval(Decimal128("2"), 0, Decimal128("2.0"));
    assertEval(Decimal128("-2"), 0, Decimal128("-2.0"));
    assertEval(Decimal128("0.9"), 0, Decimal128("0.0"));
    assertEval(Decimal128("0.1"), 0, Decimal128("0.0"));
    assertEval(Decimal128("-1.2"), 0, Decimal128("-1.0"));
    assertEval(Decimal128("-1.7"), 0, Decimal128("-1.0"));
    assertEval(Decimal128("123456789.9999999999999999999999999"), 0, Decimal128("123456789"));
    assertEval(Decimal128("-99999999999999999999999999999.99"),
               0,
               Decimal128("-99999999999999999999999999999.00"));
    assertEval(Decimal128("3.4E-6000"), 0, Decimal128("0"));

    assertEval(Decimal128("-3.14159265"), 0, Decimal128("-3.0"));
    assertEval(Decimal128("-3.14159265"), 1, Decimal128("-3.1"));
    assertEval(Decimal128("-3.14159265"), 2, Decimal128("-3.14"));
    assertEval(Decimal128("-3.14159265"), 3, Decimal128("-3.141"));
    assertEval(Decimal128("-3.14159265"), 4, Decimal128("-3.1415"));
    assertEval(Decimal128("-3.14159265"), 5, Decimal128("-3.14159"));
    assertEval(Decimal128("-3.14159265"), 6, Decimal128("-3.141592"));
    assertEval(Decimal128("-3.14159265"), 7, Decimal128("-3.1415926"));
    assertEval(Decimal128("-3.14159265"), 100, Decimal128("-3.14159265"));
    assertEval(Decimal128("3.14159265"), 0, Decimal128("3.0"));
    assertEval(Decimal128("3.14159265"), 1, Decimal128("3.1"));
    assertEval(Decimal128("3.14159265"), 2, Decimal128("3.14"));
    assertEval(Decimal128("3.14159265"), Decimal128("3"), Decimal128("3.141"));
    assertEval(Decimal128("3.14159265"), 4, Decimal128("3.1415"));
    assertEval(Decimal128("3.14159265"), Decimal128("5"), Decimal128("3.14159"));
    assertEval(Decimal128("3.14159265"), 6, Decimal128("3.141592"));
    assertEval(Decimal128("3.14159265"), 7, Decimal128("3.1415926"));
    assertEval(Decimal128("3.14159265"), 100, Decimal128("3.14159265"));
    assertEval(Decimal128("3.14159265"), Decimal128("-1"), Decimal128("0"));
    assertEval(Decimal128("335.14159265"), -1, Decimal128("330"));
    assertEval(Decimal128("333.14159265"), -2, Decimal128("300"));
}

TEST_F(ExpressionTruncOneArgTest, NullArg1) {
    assertEval((BSONNULL), (BSONNULL));
}

TEST_F(ExpressionTruncTwoArgTest, NullArg2) {
    assertEval((BSONNULL), (BSONNULL), (BSONNULL));
    assertEval((1), (BSONNULL), (BSONNULL));
    assertEval((BSONNULL), (1), (BSONNULL));
}

/* ------------------------- ExpressionSqrt -------------------------- */

class ExpressionSqrtTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionSqrt(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionSqrtTest, SqrtIntArg) {
    assertEvaluates(Value(0), Value(0.0));
    assertEvaluates(Value(1), Value(1.0));
    assertEvaluates(Value(25), Value(5.0));
}

TEST_F(ExpressionSqrtTest, SqrtLongArg) {
    assertEvaluates(Value(0LL), Value(0.0));
    assertEvaluates(Value(1LL), Value(1.0));
    assertEvaluates(Value(25LL), Value(5.0));
    assertEvaluates(Value(40000000000LL), Value(200000.0));
}

TEST_F(ExpressionSqrtTest, SqrtDoubleArg) {
    assertEvaluates(Value(0.0), Value(0.0));
    assertEvaluates(Value(1.0), Value(1.0));
    assertEvaluates(Value(25.0), Value(5.0));
}

TEST_F(ExpressionSqrtTest, SqrtDecimalArg) {
    assertEvaluates(Value(Decimal128("0")), Value(Decimal128("0")));
    assertEvaluates(Value(Decimal128("1")), Value(Decimal128("1")));
    assertEvaluates(Value(Decimal128("25")), Value(Decimal128("5")));
    assertEvaluates(Value(Decimal128("30.25")), Value(Decimal128("5.5")));
}

TEST_F(ExpressionSqrtTest, SqrtNullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
}

TEST_F(ExpressionSqrtTest, SqrtNaNArg) {
    assertEvaluates(Value(std::numeric_limits<double>::quiet_NaN()),
                    Value(std::numeric_limits<double>::quiet_NaN()));
}

/* ------------------------- ExpressionExp -------------------------- */

class ExpressionExpTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionExp(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }

    const Decimal128 decimalE = Decimal128("2.718281828459045235360287471352662");
};

TEST_F(ExpressionExpTest, ExpIntArg) {
    assertEvaluates(Value(0), Value(1.0));
    assertEvaluates(Value(1), Value(exp(1.0)));
}

TEST_F(ExpressionExpTest, ExpLongArg) {
    assertEvaluates(Value(0LL), Value(1.0));
    assertEvaluates(Value(1LL), Value(exp(1.0)));
}

TEST_F(ExpressionExpTest, ExpDoubleArg) {
    assertEvaluates(Value(0.0), Value(1.0));
    assertEvaluates(Value(1.0), Value(exp(1.0)));
}

TEST_F(ExpressionExpTest, ExpDecimalArg) {
    assertEvaluates(Value(Decimal128("0")), Value(Decimal128("1")));
    assertEvaluates(Value(Decimal128("1")), Value(decimalE));
}

TEST_F(ExpressionExpTest, ExpNullArg) {
    assertEvaluates(Value(BSONNULL), Value(BSONNULL));
}

TEST_F(ExpressionExpTest, ExpNaNArg) {
    assertEvaluates(Value(std::numeric_limits<double>::quiet_NaN()),
                    Value(std::numeric_limits<double>::quiet_NaN()));
}

/* ------------------------- ExpressionCeil -------------------------- */

class ExpressionCeilTest : public ExpressionNaryTestOneArg {
public:
    virtual void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionCeil(&expCtx);
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
    double largerThanLong = static_cast<double>(numeric_limits<long long>::max()) * 2.0;
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
        _expr = new ExpressionFloor(&expCtx);
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
    double largerThanLong = static_cast<double>(numeric_limits<long long>::max()) * 2.0;
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

/* ------------------------- ExpressionRound -------------------------- */

class ExpressionRoundOneArgTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionRound(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

class ExpressionRoundTwoArgTest : public ExpressionNaryTestTwoArg {
public:
    void assertEval(ImplicitValue input1, ImplicitValue input2, ImplicitValue output) {
        _expr = new ExpressionRound(&expCtx);
        ExpressionNaryTestTwoArg::assertEvaluates(input1, input2, output);
    }
};

TEST_F(ExpressionRoundOneArgTest, IntArg1) {
    assertEval(0, 0);
    assertEval(numeric_limits<int>::min(), numeric_limits<int>::min());
    assertEval(numeric_limits<int>::max(), numeric_limits<int>::max());
}

TEST_F(ExpressionRoundTwoArgTest, IntArg2) {
    assertEval(0, 0, 0);
    assertEval(2, -1, 0);
    assertEval(29, -1, 30);
    assertEval(numeric_limits<int>::min(), 10, numeric_limits<int>::min());
    assertEval(numeric_limits<int>::max(), 42, numeric_limits<int>::max());
}

TEST_F(ExpressionRoundOneArgTest, LongArg1) {
    assertEval(0LL, 0LL);
    assertEval(numeric_limits<long long>::min(), numeric_limits<long long>::min());
    assertEval(numeric_limits<long long>::max(), numeric_limits<long long>::max());
}

TEST_F(ExpressionRoundTwoArgTest, LongArg2) {
    assertEval(0LL, 0LL, 0LL);
    assertEval(2LL, -1LL, 0LL);
    assertEval(29LL, -1LL, 30LL);
    assertEval(numeric_limits<long long>::min(), 10LL, numeric_limits<long long>::min());
    assertEval(numeric_limits<long long>::max(), 42LL, numeric_limits<long long>::max());
}

TEST_F(ExpressionRoundOneArgTest, DoubleArg1) {
    assertEval(2.0, 2.0);
    assertEval(-2.0, -2.0);
    assertEval(0.9, 1.0);
    assertEval(0.1, 0.0);
    assertEval(1.5, 2.0);
    assertEval(2.5, 2.0);
    assertEval(3.5, 4.0);
    assertEval(-1.2, -1.0);
    assertEval(-1.7, -2.0);
    assertEval(-1.5, -2.0);
    assertEval(-2.5, -2.0);

    // Outside the range of long longs (there isn't enough precision for decimals in this range, so
    // should just preserve the number).
    double largerThanLong = static_cast<double>(numeric_limits<long long>::max()) * 2.0;
    assertEval(largerThanLong, largerThanLong);
    double smallerThanLong = numeric_limits<long long>::min() * 2.0;
    assertEval(smallerThanLong, smallerThanLong);
}

TEST_F(ExpressionRoundTwoArgTest, DoubleArg2) {
    assertEval(2.0, 1.0, 2.0);
    assertEval(-2.0, 2.0, -2.0);
    assertEval(0.9, 0, 1.0);
    assertEval(0.1, 0, 0.0);
    assertEval(1.5, 0, 2.0);
    assertEval(2.5, 0, 2.0);
    assertEval(3.5, 0, 4.0);
    assertEval(-1.2, 0, -1.0);
    assertEval(-1.7, 0, -2.0);
    assertEval(-1.5, 0, -2.0);
    assertEval(-2.5, 0, -2.0);

    assertEval(-3.14159265, 0, -3.0);
    assertEval(-3.14159265, 1, -3.1);
    assertEval(-3.14159265, 2, -3.14);
    assertEval(-3.14159265, 3, -3.142);
    assertEval(-3.14159265, 4, -3.1416);
    assertEval(-3.14159265, 5, -3.14159);
    assertEval(-3.14159265, 6, -3.141593);
    assertEval(-3.14159265, 7, -3.1415927);
    assertEval(-3.14159265, 100, -3.14159265);
    assertEval(3.14159265, 0, 3.0);
    assertEval(3.14159265, 1, 3.1);
    assertEval(3.14159265, 2, 3.14);
    assertEval(3.14159265, 3, 3.142);
    assertEval(3.14159265, 4, 3.1416);
    assertEval(3.14159265, 5, 3.14159);
    assertEval(3.14159265, 6, 3.141593);
    assertEval(3.14159265, 7, 3.1415927);
    assertEval(3.14159265, 100, 3.14159265);
    assertEval(3.14159265, -1, 0.0);
    assertEval(335.14159265, -1, 340.0);
    assertEval(333.14159265, -2, 300.0);
}

TEST_F(ExpressionRoundOneArgTest, DecimalArg1) {
    assertEval(Decimal128("2"), Decimal128("2.0"));
    assertEval(Decimal128("-2"), Decimal128("-2.0"));
    assertEval(Decimal128("0.9"), Decimal128("1.0"));
    assertEval(Decimal128("0.1"), Decimal128("0.0"));
    assertEval(Decimal128("0.5"), Decimal128("0.0"));
    assertEval(Decimal128("1.5"), Decimal128("2.0"));
    assertEval(Decimal128("2.5"), Decimal128("2.0"));
    assertEval(Decimal128("-1.2"), Decimal128("-1.0"));
    assertEval(Decimal128("-1.7"), Decimal128("-2.0"));
    assertEval(Decimal128("123456789.9999999999999999999999999"), Decimal128("123456790"));
    assertEval(Decimal128("-99999999999999999999999999999.99"),
               Decimal128("-100000000000000000000000000000"));
    assertEval(Decimal128("3.4E-6000"), Decimal128("0"));
}

TEST_F(ExpressionRoundTwoArgTest, DecimalArg2) {
    assertEval(Decimal128("2"), 0, Decimal128("2.0"));
    assertEval(Decimal128("-2"), 0, Decimal128("-2.0"));
    assertEval(Decimal128("0.9"), 0, Decimal128("1.0"));
    assertEval(Decimal128("0.1"), 0, Decimal128("0.0"));
    assertEval(Decimal128("0.5"), 0, Decimal128("0.0"));
    assertEval(Decimal128("1.5"), 0, Decimal128("2.0"));
    assertEval(Decimal128("2.5"), 0, Decimal128("2.0"));
    assertEval(Decimal128("-1.2"), 0, Decimal128("-1.0"));
    assertEval(Decimal128("-1.7"), 0, Decimal128("-2.0"));
    assertEval(Decimal128("123456789.9999999999999999999999999"), 0, Decimal128("123456790"));
    assertEval(Decimal128("-99999999999999999999999999999.99"),
               0,
               Decimal128("-100000000000000000000000000000"));
    assertEval(Decimal128("3.4E-6000"), 0, Decimal128("0"));

    assertEval(Decimal128("-3.14159265"), 0, Decimal128("-3.0"));
    assertEval(Decimal128("-3.14159265"), 1, Decimal128("-3.1"));
    assertEval(Decimal128("-3.14159265"), 2, Decimal128("-3.14"));
    assertEval(Decimal128("-3.14159265"), 3, Decimal128("-3.142"));
    assertEval(Decimal128("-3.14159265"), 4, Decimal128("-3.1416"));
    assertEval(Decimal128("-3.14159265"), 5, Decimal128("-3.14159"));
    assertEval(Decimal128("-3.14159265"), 6, Decimal128("-3.141593"));
    assertEval(Decimal128("-3.14159265"), 7, Decimal128("-3.1415926"));
    assertEval(Decimal128("-3.14159265"), 100, Decimal128("-3.14159265"));
    assertEval(Decimal128("3.14159265"), 0, Decimal128("3.0"));
    assertEval(Decimal128("3.14159265"), 1, Decimal128("3.1"));
    assertEval(Decimal128("3.14159265"), 2, Decimal128("3.14"));
    assertEval(Decimal128("3.14159265"), Decimal128("3"), Decimal128("3.142"));
    assertEval(Decimal128("3.14159265"), 4, Decimal128("3.1416"));
    assertEval(Decimal128("3.14159265"), Decimal128("5"), Decimal128("3.14159"));
    assertEval(Decimal128("3.14159265"), 6, Decimal128("3.141593"));
    assertEval(Decimal128("3.14159265"), 7, Decimal128("3.1415926"));
    assertEval(Decimal128("3.14159265"), 100, Decimal128("3.14159265"));
    assertEval(Decimal128("3.14159265"), Decimal128("-1"), Decimal128("0"));
    assertEval(Decimal128("335.14159265"), -1, Decimal128("340"));
    assertEval(Decimal128("333.14159265"), -2, Decimal128("300"));
}

TEST_F(ExpressionRoundOneArgTest, NullArg1) {
    assertEval(BSONNULL, BSONNULL);
}

TEST_F(ExpressionRoundTwoArgTest, NullArg2) {
    assertEval(BSONNULL, BSONNULL, BSONNULL);
    assertEval(1, BSONNULL, BSONNULL);
    assertEval(BSONNULL, 1, BSONNULL);
}

/* ------------------------- ExpressionBinarySize -------------------------- */

class ExpressionBinarySizeTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionBinarySize(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionBinarySizeTest, HandlesStrings) {
    assertEval("abc"_sd, 3);
    assertEval(""_sd, 0);
    assertEval("ab\0c"_sd, 4);
    assertEval("abc\0"_sd, 4);
}

TEST_F(ExpressionBinarySizeTest, HandlesBinData) {
    assertEval(BSONBinData("abc", 3, BinDataGeneral), 3);
    assertEval(BSONBinData("", 0, BinDataGeneral), 0);
    assertEval(BSONBinData("ab\0c", 4, BinDataGeneral), 4);
    assertEval(BSONBinData("abc\0", 4, BinDataGeneral), 4);
}

TEST_F(ExpressionBinarySizeTest, HandlesNullish) {
    assertEval(BSONNULL, BSONNULL);
    assertEval(BSONUndefined, BSONNULL);
    assertEval(Value(), BSONNULL);
}

/* ------------------------- ExpressionFirst -------------------------- */

class ExpressionFirstTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionFirst(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
    void assertEvalFails(ImplicitValue input) {
        _expr = new ExpressionFirst(&expCtx);
        ASSERT_THROWS_CODE(eval(input), DBException, 28689);
    }
};

TEST_F(ExpressionFirstTest, HandlesArrays) {
    assertEval(vector<Value>{Value("A"_sd)}, "A"_sd);
    assertEval(vector<Value>{Value("A"_sd), Value("B"_sd)}, "A"_sd);
    assertEval(vector<Value>{Value("A"_sd), Value("B"_sd), Value("C"_sd)}, "A"_sd);
}

TEST_F(ExpressionFirstTest, HandlesEmptyArray) {
    assertEval(vector<Value>{}, Value());
}

TEST_F(ExpressionFirstTest, HandlesNullish) {
    assertEval(BSONNULL, BSONNULL);
    assertEval(BSONUndefined, BSONNULL);
    assertEval(Value(), BSONNULL);
}

TEST_F(ExpressionFirstTest, RejectsNonArrays) {
    assertEvalFails("asdf"_sd);
    assertEvalFails(BSONBinData("asdf", 4, BinDataGeneral));
}

/* ------------------------- ExpressionLast -------------------------- */

class ExpressionLastTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionLast(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
    void assertEvalFails(ImplicitValue input) {
        _expr = new ExpressionLast(&expCtx);
        ASSERT_THROWS_CODE(eval(input), DBException, 28689);
    }
};

TEST_F(ExpressionLastTest, HandlesArrays) {
    assertEval(vector<Value>{Value("A"_sd)}, "A"_sd);
    assertEval(vector<Value>{Value("A"_sd), Value("B"_sd)}, "B"_sd);
    assertEval(vector<Value>{Value("A"_sd), Value("B"_sd), Value("C"_sd)}, "C"_sd);
}

TEST_F(ExpressionLastTest, HandlesEmptyArray) {
    assertEval(vector<Value>{}, Value());
}

TEST_F(ExpressionLastTest, HandlesNullish) {
    assertEval(BSONNULL, BSONNULL);
    assertEval(BSONUndefined, BSONNULL);
    assertEval(Value(), BSONNULL);
}

TEST_F(ExpressionLastTest, RejectsNonArrays) {
    assertEvalFails("asdf"_sd);
    assertEvalFails(BSONBinData("asdf", 4, BinDataGeneral));
}

/* ------------------------- ExpressionTsSecond -------------------------- */

class ExpressionTsSecondTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionTsSecond(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
    void assertEvalFails(ImplicitValue input) {
        _expr = new ExpressionTsSecond(&expCtx);
        ASSERT_THROWS_CODE(eval(input), DBException, 5687301);
    }
};

TEST_F(ExpressionTsSecondTest, HandlesTimestamp) {
    assertEval(Timestamp(1622731060, 10), static_cast<long long>(1622731060));
}

TEST_F(ExpressionTsSecondTest, HandlesNullish) {
    assertEval(BSONNULL, BSONNULL);
    assertEval(BSONUndefined, BSONNULL);
    assertEval(Value(), BSONNULL);
}

TEST_F(ExpressionTsSecondTest, HandlesInvalidTimestamp) {
    assertEvalFails(1622731060);
}

/* ------------------------- ExpressionTsIncrement -------------------------- */

class ExpressionTsIncrementTest : public ExpressionNaryTestOneArg {
public:
    void assertEval(ImplicitValue input, ImplicitValue output) {
        _expr = new ExpressionTsIncrement(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
    void assertEvalFails(ImplicitValue input) {
        _expr = new ExpressionTsIncrement(&expCtx);
        ASSERT_THROWS_CODE(eval(input), DBException, 5687302);
    }
};

TEST_F(ExpressionTsIncrementTest, HandlesTimestamp) {
    assertEval(Timestamp(1622731060, 10), static_cast<long long>(10));
}

TEST_F(ExpressionTsIncrementTest, HandlesNullish) {
    assertEval(BSONNULL, BSONNULL);
    assertEval(BSONUndefined, BSONNULL);
    assertEval(Value(), BSONNULL);
}

TEST_F(ExpressionTsIncrementTest, HandlesInvalidTimestamp) {
    assertEvalFails(10);
}

}  // anonymous namespace
}  // namespace ExpressionTests

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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cmath>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

using namespace mongo;

namespace ExpressionTests {

/** A dummy child of ExpressionNary used for testing. */
class Testable : public ExpressionNary {
public:
    Value evaluate(const Document& root, Variables* variables) const override {
        // Just put all the values in a list.
        // By default, this is not associative/commutative so the results will change if
        // instantiated as commutative or associative and operations are reordered.
        std::vector<Value> values;
        for (auto&& child : _children)
            values.push_back(child->evaluate(root, variables));
        return Value(values);
    }

    const char* getOpName() const override {
        return "$testable";
    }

    Associativity getAssociativity() const override {
        return _associativity;
    }

    bool isCommutative() const override {
        return _isCommutative;
    }

    void acceptVisitor(ExpressionMutableVisitor* visitor) final {
        return visitor->visit(this);
    }

    void acceptVisitor(ExpressionConstVisitor* visitor) const final {
        return visitor->visit(this);
    }

    static boost::intrusive_ptr<Testable> create(ExpressionContext* const expCtx,
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

/** Convert Expression to BSON. */
static BSONObj expressionToBson(const boost::intrusive_ptr<Expression>& expression) {
    return BSON("" << expression->serialize()).firstElement().embeddedObject().getOwned();
}

/* ------------------------- NaryExpression -------------------------- */

class ExpressionNaryTest : public unittest::Test {
public:
    void setUp() override {
        _notAssociativeNorCommutative =
            Testable::create(&expCtx, ExpressionNary::Associativity::kNone, false);
        _associativeOnly = Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
        _associativeAndCommutative =
            Testable::create(&expCtx, ExpressionNary::Associativity::kFull, true);
        _leftAssociativeOnly =
            Testable::create(&expCtx, ExpressionNary::Associativity::kLeft, false);
    }

protected:
    void assertDependencies(const boost::intrusive_ptr<Testable>& expr,
                            const BSONArray& expectedDependencies) {
        DepsTracker dependencies;
        expression::addDependencies(expr.get(), &dependencies);
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

    void assertContents(const boost::intrusive_ptr<Testable>& expr,
                        const BSONArray& expectedContents) {
        ASSERT_BSONOBJ_EQ(constify(BSON("$testable" << expectedContents)), expressionToBson(expr));
    }

    void addOperandArrayToExpr(const boost::intrusive_ptr<Testable>& expr,
                               const BSONArray& operands) {
        VariablesParseState vps = expCtx.variablesParseState;
        BSONObjIterator i(operands);
        while (i.more()) {
            BSONElement element = i.next();
            expr->addOperand(Expression::parseOperand(&expCtx, element, vps));
        }
    }

    ExpressionContextForTest expCtx;
    boost::intrusive_ptr<Testable> _notAssociativeNorCommutative;
    boost::intrusive_ptr<Testable> _associativeOnly;
    boost::intrusive_ptr<Testable> _associativeAndCommutative;
    boost::intrusive_ptr<Testable> _leftAssociativeOnly;
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
    BSONObj spec = BSON("" << BSON("a" << "$x"
                                       << "q"
                                       << "$r"));
    BSONElement specElement = spec.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    _notAssociativeNorCommutative->addOperand(
        Expression::parseObject(&expCtx, specElement.Obj(), vps));
    assertDependencies(_notAssociativeNorCommutative, BSON_ARRAY("r" << "x"));
}

TEST_F(ExpressionNaryTest, SerializationToBsonObj) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5)))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize()));
}

TEST_F(ExpressionNaryTest, SerializationToBsonArr) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
    ASSERT_BSONOBJ_EQ(constify(BSON_ARRAY(BSON("$testable" << BSON_ARRAY(5)))),
                      BSON_ARRAY(_notAssociativeNorCommutative->serialize()));
}

TEST_F(ExpressionNaryTest, RedactsCorrectlyWithConstantArguments) {
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(10)));
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(15)));

    SerializationOptions opts;

    // The default shape should wrap the constants in $const.
    ASSERT_BSONOBJ_EQ(
        BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5) << BSON("$const" << 10)
                                                                         << BSON("$const" << 15)))),
        BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));

    // The representative shape should be an array of raw constants (i.e. not wrapped in $const).
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(1 << 1 << 1))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));
}

TEST_F(ExpressionNaryTest, RedactsCorrectlyWithMixedArguments) {
    VariablesParseState vps = expCtx.variablesParseState;
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
    _notAssociativeNorCommutative->addOperand(
        Expression::parseExpression(&expCtx, BSON("$sum" << BSON_ARRAY(1 << 2)), vps));
    _notAssociativeNorCommutative->addOperand(ExpressionFieldPath::parse(&expCtx, "$b", vps));

    SerializationOptions opts;

    // The default shape should wrap the constants in $const.
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(
                                             BSON("$const" << 5)
                                             << BSON("$sum" << BSON_ARRAY(BSON("$const" << 1)
                                                                          << BSON("$const" << 2)))
                                             << "$b"))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));

    // The representative shape should not wrap the constant in $const.
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(
                                             1 << BSON("$sum" << BSON_ARRAY(1 << 1)) << "$b"))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));
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
    boost::intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
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
    boost::intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
    ASSERT(_notAssociativeNorCommutative == optimized);
    assertContents(_notAssociativeNorCommutative, spec);
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyFrontOperands) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnLeftAssociativeOnlyFrontOperands) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(_leftAssociativeOnly, BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path"));
}


TEST_F(ExpressionNaryTest, GroupingOptimizationOnlyExecuteOnLeftAssociativeOnlyFrontConstants) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path" << 77 << 88 << "$path1" << 99 << 1010);
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(
        _leftAssociativeOnly,
        BSON_ARRAY(BSON_ARRAY(55 << 66) << "$path" << 77 << 88 << "$path1" << 99 << 1010));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyMiddleOperands) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << 66 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << BSON_ARRAY(55 << 66) << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationNotExecuteOnLeftAssociativeOnlyMiddleOperands) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << 66 << "$path");
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(_leftAssociativeOnly, BSON_ARRAY("$path1" << 55 << 66 << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyBackOperands) {
    BSONArray spec = BSON_ARRAY("$path" << 55 << 66);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationNotExecuteOnLeftAssociativeOnlyBackOperands) {
    BSONArray spec = BSON_ARRAY("$path" << 55 << 66);
    addOperandArrayToExpr(_leftAssociativeOnly, spec);
    assertContents(_leftAssociativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
    ASSERT(_leftAssociativeOnly == optimized);
    assertContents(_leftAssociativeOnly, BSON_ARRAY("$path" << 55 << 66));
}


TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsFront) {
    BSONArray spec = BSON_ARRAY(55 << "$path");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY(55 << "$path"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsMiddle) {
    BSONArray spec = BSON_ARRAY("$path1" << 55 << "$path2");
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path1" << 55 << "$path2"));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnAssociativeOnlyNotExecuteOnSingleConstantsBack) {
    BSONArray spec = BSON_ARRAY("$path" << 55);
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, BSON_ARRAY("$path" << 55));
}

TEST_F(ExpressionNaryTest, GroupingOptimizationOnCommutativeAndAssociative) {
    BSONArray spec = BSON_ARRAY(55 << 66 << "$path");
    addOperandArrayToExpr(_associativeAndCommutative, spec);
    assertContents(_associativeAndCommutative, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeAndCommutative->optimize();
    ASSERT(_associativeAndCommutative == optimized);
    assertContents(_associativeAndCommutative, BSON_ARRAY("$path" << BSON_ARRAY(55 << 66)));
}

TEST_F(ExpressionNaryTest, FlattenOptimizationNotDoneOnOtherExpressionsForAssociativeExpressions) {
    BSONArray spec = BSON_ARRAY(66 << "$path" << BSON("$sum" << BSON_ARRAY("$path" << 2)));
    addOperandArrayToExpr(_associativeOnly, spec);
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);
    assertContents(_associativeOnly, spec);
}


TEST_F(ExpressionNaryTest, FlattenOptimizationNotDoneOnSameButNotAssociativeExpression) {
    BSONArrayBuilder specBuilder;

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kNone, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    assertContents(_associativeOnly, spec);
}

// Test that if there is an expression of the same type in a non-commutative nor associative
// expression, the inner expression is not expanded.
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] } is optimized to:
// {"$testable" : [ { "$testable" : [ 100, "$path1"] }, 99, "$path2"] }
TEST_F(ExpressionNaryTest, FlattenInnerOperandsOptimizationOnNotCommutativeNorAssociative) {
    BSONArrayBuilder specBuilder;

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kNone, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _notAssociativeNorCommutative->addOperand(innerOperand);

    addOperandArrayToExpr(_notAssociativeNorCommutative, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_notAssociativeNorCommutative, spec);
    boost::intrusive_ptr<Expression> optimized = _notAssociativeNorCommutative->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
    ASSERT(_associativeOnly == optimized);

    BSONArray expectedContent = BSON_ARRAY(100 << "$path1" << BSON_ARRAY(101 << 99) << "$path2");
    assertContents(_associativeOnly, expectedContent);
}


// Test that if there is an expression of the same type as the first operand
// in a non-commutative but left-associative expression, the inner expression is not expanded.
TEST_F(ExpressionNaryTest,
       FlattenInnerOperandsOptimizationOnLeftAssociativeFrontOperandAndGroupIsNoOp) {
    BSONArrayBuilder specBuilder;

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kLeft, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY("$path" << 100 << 200 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _leftAssociativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_leftAssociativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_leftAssociativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _leftAssociativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeOnly, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1"));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    boost::intrusive_ptr<Testable> innerOperand2 =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand2, BSON_ARRAY(200 << "$path2"));
    specBuilder.append(expressionToBson(innerOperand2));
    _associativeOnly->addOperand(innerOperand2);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeOnly->addOperand(innerOperand);

    boost::intrusive_ptr<Testable> innerOperand2 =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, false);
    addOperandArrayToExpr(innerOperand2, BSON_ARRAY(200 << "$path2"));
    specBuilder.append(expressionToBson(innerOperand2));
    _associativeOnly->addOperand(innerOperand2);

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeOnly, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeOnly->optimize();
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

    boost::intrusive_ptr<Testable> innerOperand =
        Testable::create(&expCtx, ExpressionNary::Associativity::kFull, true);
    addOperandArrayToExpr(innerOperand, BSON_ARRAY(100 << "$path1" << 101));
    specBuilder.append(expressionToBson(innerOperand));
    _associativeAndCommutative->addOperand(innerOperand);

    addOperandArrayToExpr(_associativeAndCommutative, BSON_ARRAY(99 << "$path2"));
    specBuilder << 99 << "$path2";

    BSONArray spec = specBuilder.arr();
    assertContents(_associativeAndCommutative, spec);
    boost::intrusive_ptr<Expression> optimized = _associativeAndCommutative->optimize();
    ASSERT(_associativeAndCommutative == optimized);

    BSONArray expectedContent =
        BSON_ARRAY("$path3" << "$path1"
                            << "$path2" << BSON_ARRAY(200 << 201 << BSON_ARRAY(100 << 101) << 99));
    assertContents(_associativeAndCommutative, expectedContent);
}

}  // anonymous namespace
}  // namespace ExpressionTests

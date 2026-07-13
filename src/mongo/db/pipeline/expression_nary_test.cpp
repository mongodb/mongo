// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

using namespace mongo;

namespace ExpressionTests {

/** A dummy child of ExpressionNary used for testing. */
class Testable : public ExpressionNary {
public:
    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const override {
        // Just put all the values in a list.
        // By default, this is not associative/commutative so the results will change if
        // instantiated as commutative or associative and operations are reordered.
        std::vector<Value> values;
        for (auto&& child : _children)
            values.push_back(child->evaluate(root, variables, ctx));
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


    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return Testable::create(&expCtx, _associativity, _isCommutative);
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
        ExpressionFieldPath::createPathFromString(&expCtx, "ab.c", expCtx.variablesParseState));
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
        ExpressionFieldPath::createPathFromString(&expCtx, "ab.c", expCtx.variablesParseState));
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

    query_shape::SerializationOptions opts;

    // The default shape should wrap the constants in $const.
    ASSERT_BSONOBJ_EQ(
        BSON("foo" << BSON("$testable" << BSON_ARRAY(BSON("$const" << 5) << BSON("$const" << 10)
                                                                         << BSON("$const" << 15)))),
        BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));

    // The representative shape should be an array of raw constants (i.e. not wrapped in $const).
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(1 << 1 << 1))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));
}

TEST_F(ExpressionNaryTest, RedactsCorrectlyWithMixedArguments) {
    VariablesParseState vps = expCtx.variablesParseState;
    _notAssociativeNorCommutative->addOperand(ExpressionConstant::create(&expCtx, Value(5)));
    _notAssociativeNorCommutative->addOperand(
        Expression::parseExpression(&expCtx, BSON("$sum" << BSON_ARRAY(1 << 2)), vps));
    _notAssociativeNorCommutative->addOperand(ExpressionFieldPath::parse(&expCtx, "$b", vps));

    query_shape::SerializationOptions opts;

    // The default shape should wrap the constants in $const.
    ASSERT_BSONOBJ_EQ(BSON("foo" << BSON("$testable" << BSON_ARRAY(
                                             BSON("$const" << 5)
                                             << BSON("$sum" << BSON_ARRAY(BSON("$const" << 1)
                                                                          << BSON("$const" << 2)))
                                             << "$b"))),
                      BSON("foo" << _notAssociativeNorCommutative->serialize(opts)));

    // The representative shape should not wrap the constant in $const.
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;
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

TEST_F(ExpressionNaryTest, ConstantFoldingObeysExpressionCapWithoutOperationContext) {
    unittest::ServerParameterGuard queryFlag("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprFlag("featureFlagExpressionMemoryTracking", true);
    // Constant folding evaluates with a default EvaluationContext, so it charges the
    // ExpressionContext's fallback tracker. With no OperationContext that fallback is a standalone
    // tracker bounded by the per-expression cap, which is the only limit folding can exceed here.
    unittest::ServerParameterGuard exprCap("internalQueryMaxSingleExpressionMemoryUsageBytes",
                                           100LL);

    BSONArrayBuilder arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.append(i);
    for (int i = 10; i < 20; ++i)
        arr2.append(i);
    auto expr =
        Expression::parseExpression(&expCtx,
                                    BSON("$concatArrays" << BSON_ARRAY(arr1.arr() << arr2.arr())),
                                    expCtx.variablesParseState);

    // Null the opCtx so the fallback tracker is standalone (bounded by the per-expression cap).
    auto* savedOpCtx = expCtx.getOperationContext();
    expCtx.setOperationContext(nullptr);
    ON_BLOCK_EXIT([&] { expCtx.setOperationContext(savedOpCtx); });

    ASSERT_THROWS_CODE(expr->optimize(), AssertionException, ErrorCodes::ExceededMemoryLimit);
}

TEST_F(ExpressionNaryTest, ConstantFoldingIsEnforcedAgainstOperationLimitAtNextCheck) {
    unittest::ServerParameterGuard queryFlag("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprFlag("featureFlagExpressionMemoryTracking", true);
    // Constant folding may run before query settings are applied to the operation, so optimize()
    // itself must not resolve the operation-wide limit. The folded value's footprint stays
    // charged to the operation, so the next ordinary limit check, which runs once query settings
    // are applied, enforces the tiny per-operation limit against the fold.
    QueryKnobGuardForTest limitGuard(
        expCtx.getOperationContext(), "internalQueryMaxMemoryUsageBytesPerOperation", 100LL);
    // The opCtx-path fallback is chunked; disable chunking so the small folded array propagates to
    // the operation tracker immediately instead of being buffered below the chunk threshold.
    unittest::ServerParameterGuard chunkSize("internalQueryMaxWriteToCurOpMemoryUsageBytes", 0);

    BSONArrayBuilder arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.append(i);
    for (int i = 10; i < 20; ++i)
        arr2.append(i);
    auto expr =
        Expression::parseExpression(&expCtx,
                                    BSON("$concatArrays" << BSON_ARRAY(arr1.arr() << arr2.arr())),
                                    expCtx.variablesParseState);

    auto folded = expr->optimize();
    ASSERT(dynamic_cast<ExpressionConstant*>(folded.get()));

    auto& fallbackTracker = expCtx.getExpressionFallbackTracker();
    ASSERT_GT(fallbackTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_THROWS_CODE(
        fallbackTracker.assertWithinMemoryLimit(expCtx.getOperationContext(), "constant folding"),
        AssertionException,
        ErrorCodes::ExceededMemoryLimit);
}

TEST_F(ExpressionNaryTest, LetParameterSeedingIsEnforcedAgainstOperationLimitAtNextCheck) {
    unittest::ServerParameterGuard queryFlag("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprFlag("featureFlagExpressionMemoryTracking", true);
    // 'let' parameters are evaluated at ExpressionContext construction, before query settings are
    // applied, so seeding itself must not resolve the operation-wide limit. The seeded value
    // lives on for the whole operation, so its footprint stays charged and the next ordinary
    // limit check enforces the tiny per-operation limit against it.
    QueryKnobGuardForTest limitGuard(
        expCtx.getOperationContext(), "internalQueryMaxMemoryUsageBytesPerOperation", 100LL);
    // The opCtx-path fallback is chunked; disable chunking so the small seeded array propagates
    // to the operation tracker immediately instead of being buffered below the chunk threshold.
    unittest::ServerParameterGuard chunkSize("internalQueryMaxWriteToCurOpMemoryUsageBytes", 0);

    BSONArrayBuilder arr;
    for (int i = 0; i < 20; ++i)
        arr.append(i);
    expCtx.variables.seedVariablesWithLetParameters(
        &expCtx,
        BSON("c" << BSON("$concatArrays" << BSON_ARRAY(arr.arr()))),
        [](const Expression*) { return true; });

    auto& fallbackTracker = expCtx.getExpressionFallbackTracker();
    ASSERT_GT(fallbackTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_THROWS_CODE(
        fallbackTracker.assertWithinMemoryLimit(expCtx.getOperationContext(), "let seeding"),
        AssertionException,
        ErrorCodes::ExceededMemoryLimit);
}

}  // anonymous namespace
}  // namespace ExpressionTests

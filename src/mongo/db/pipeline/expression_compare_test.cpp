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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ExpressionTests {
namespace {

using boost::intrusive_ptr;

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

namespace Compare {

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        auto expCtx = ExpressionContextForTest{};
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
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
        auto expCtx = ExpressionContextForTest{};
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        // Check expression spec round trip.
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        // Check evaluation result.
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(expression->evaluate({}, &expCtx.variables)));
        // Check that the result is the same after optimizing.
        intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(expectedResult(), toBson(optimized->evaluate({}, &expCtx.variables)));
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
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        ASSERT_THROWS(Expression::parseOperand(&expCtx, specElement, vps), AssertionException);
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
        auto expCtx = ExpressionContextForTest{};
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_VALUE_EQ(expression->evaluate({}, &expCtx.variables), Value(true));
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

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() {
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
    }
};

OldStyleSuiteInitializer<All> compareAll;

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

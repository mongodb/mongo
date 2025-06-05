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
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace ExpressionTests {
namespace {

using boost::intrusive_ptr;

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
    BSONObj expectedOptimized() override {
        return spec();
    }
};

class NoOptimize : public OptimizeBase {
    BSONObj expectedOptimized() override {
        return spec();
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

/** Zero operands provided. */
class ZeroOperands : public ParseError {
    BSONObj spec() override {
        return BSON("$ne" << BSONArray());
    }
};

/** One operand provided. */
class OneOperand : public ParseError {
    BSONObj spec() override {
        return BSON("$eq" << BSON_ARRAY(1));
    }
};

/** Three operands provided. */
class ThreeOperands : public ParseError {
    BSONObj spec() override {
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
    BSONObj spec() override {
        return BSON("$eq" << BSON_ARRAY(1 << 1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** $cmp is not optimized. */
class NoOptimizeCmp : public NoOptimize {
    BSONObj spec() override {
        return BSON("$cmp" << BSON_ARRAY(1 << "$a"));
    }
};

/** $ne is not optimized. */
class NoOptimizeNe : public NoOptimize {
    BSONObj spec() override {
        return BSON("$ne" << BSON_ARRAY(1 << "$a"));
    }
};

/** No optimization is performend without a constant. */
class NoOptimizeNoConstant : public NoOptimize {
    BSONObj spec() override {
        return BSON("$ne" << BSON_ARRAY("$a" << "$b"));
    }
};

/** No optimization is performend without an immediate field path. */
class NoOptimizeWithoutFieldPath : public NoOptimize {
    BSONObj spec() override {
        return BSON("$eq" << BSON_ARRAY(BSON("$and" << BSON_ARRAY("$a")) << 1));
    }
};

/** No optimization is performend without an immediate field path. */
class NoOptimizeWithoutFieldPathReverse : public NoOptimize {
    BSONObj spec() override {
        return BSON("$eq" << BSON_ARRAY(1 << BSON("$and" << BSON_ARRAY("$a"))));
    }
};

/** An equality expression is optimized. */
class OptimizeEq : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$eq" << BSON_ARRAY("$a" << 1));
    }
};

/** A reverse sense equality expression is optimized. */
class OptimizeEqReverse : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$eq" << BSON_ARRAY(1 << "$a"));
    }
};

/** A $lt expression is optimized. */
class OptimizeLt : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$lt" << BSON_ARRAY("$a" << 1));
    }
};

/** A reverse sense $lt expression is optimized. */
class OptimizeLtReverse : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$lt" << BSON_ARRAY(1 << "$a"));
    }
};

/** A $lte expression is optimized. */
class OptimizeLte : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$lte" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $lte expression is optimized. */
class OptimizeLteReverse : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$lte" << BSON_ARRAY(2 << "$b"));
    }
};

/** A $gt expression is optimized. */
class OptimizeGt : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$gt" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $gt expression is optimized. */
class OptimizeGtReverse : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$gt" << BSON_ARRAY(2 << "$b"));
    }
};

/** A $gte expression is optimized. */
class OptimizeGte : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$gte" << BSON_ARRAY("$b" << 2));
    }
};

/** A reverse sense $gte expression is optimized. */
class OptimizeGteReverse : public FieldRangeOptimize {
    BSONObj spec() override {
        return BSON("$gte" << BSON_ARRAY(2 << "$b"));
    }
};

}  // namespace Compare

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() override {
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

unittest::OldStyleSuiteInitializer<All> compareAll;

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

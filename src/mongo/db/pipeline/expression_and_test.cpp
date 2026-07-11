// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace ExpressionTests {
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

namespace And {

class ExpectedParsedBase {
public:
    virtual ~ExpectedParsedBase() {}
    void run() {
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        boost::intrusive_ptr<Expression> expression =
            Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
    }

protected:
    virtual BSONObj spec() = 0;
};

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        boost::intrusive_ptr<Expression> expression =
            Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        boost::intrusive_ptr<Expression> optimized = expression->optimize();
        ASSERT_BSONOBJ_EQ(expectedOptimized(), expressionToBson(optimized));
    }

protected:
    virtual BSONObj spec() = 0;
    virtual BSONObj expectedOptimized() = 0;
};

class NoOptimizeBase : public OptimizeBase {
    BSONObj expectedOptimized() override {
        return constify(spec());
    }
};

/** $and without operands. */
class NoOperands : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSONArray());
    }
};

/** $and passed 'true'. */
class True : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(true));
    }
};

/** $and passed 'false'. */
class False : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(false));
    }
};

/** $and passed 'true', 'true'. */
class TrueTrue : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(true << true));
    }
};

/** $and passed 'true', 'false'. */
class TrueFalse : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(true << false));
    }
};

/** $and passed 'false', 'true'. */
class FalseTrue : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(false << true));
    }
};

/** $and passed 'false', 'false'. */
class FalseFalse : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(false << false));
    }
};

/** $and passed 'true', 'true', 'true'. */
class TrueTrueTrue : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(true << true << true));
    }
};

/** $and passed 'true', 'true', 'false'. */
class TrueTrueFalse : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(true << true << false));
    }
};

/** $and passed '0', '1'. */
class ZeroOne : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(0 << 1));
    }
};

/** $and passed '1', '2'. */
class OneTwo : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(1 << 2));
    }
};

/** $and passed a field path. */
class FieldPath : public ExpectedParsedBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** A constant expression is optimized to a constant. */
class OptimizeConstantExpression : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** A non constant expression is not optimized. */
class NonConstant : public NoOptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantTrue : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(1 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

class ConstantNonConstantFalse : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(0 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << false);
    }
};

/** An expression with a field path and '1'. */
class NonConstantOne : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY("$a" << 1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** An expression with a field path and '0'. */
class NonConstantZero : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY("$a" << 0));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << false);
    }
};

/** An expression with two field paths and '1'. */
class NonConstantNonConstantOne : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY("$a" << "$b" << 1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$and" << BSON_ARRAY("$a" << "$b"));
    }
};

/** An expression with two field paths and '0'. */
class NonConstantNonConstantZero : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY("$a" << "$b" << 0));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << false);
    }
};

/** An expression with '0', '1', and a field path. */
class ZeroOneNonConstant : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(0 << 1 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << false);
    }
};

/** An expression with '1', '1', and a field path. */
class OneOneNonConstant : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(1 << 1 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$and" << BSON_ARRAY("$a"));
    }
};

/** Nested $and expressions. */
class Nested : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(1 << BSON("$and" << BSON_ARRAY(1)) << "$a"
                                           << "$b"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$and" << BSON_ARRAY("$a" << "$b"));
    }
};

/** Nested $and expressions containing a nested value evaluating to false. */
class NestedZero : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$and" << BSON_ARRAY(
                        1 << BSON("$and" << BSON_ARRAY(BSON("$and" << BSON_ARRAY(0)))) << "$a"
                          << "$b"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << false);
    }
};

}  // namespace And

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() override {
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
    }
};

unittest::OldStyleSuiteInitializer<All> andAll;

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

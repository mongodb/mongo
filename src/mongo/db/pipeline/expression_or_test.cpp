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

/** Create a Document from a BSONObj. */
Document fromBson(BSONObj obj) {
    return Document(obj);
}

namespace Or {

class OptimizeBase {
public:
    virtual ~OptimizeBase() {}
    void run() {
        auto expCtx = ExpressionContextForTest{};
        BSONObj specObject = BSON("" << spec());
        BSONElement specElement = specObject.firstElement();
        VariablesParseState vps = expCtx.variablesParseState;
        intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
        ASSERT_BSONOBJ_EQ(constify(spec()), expressionToBson(expression));
        intrusive_ptr<Expression> optimized = expression->optimize();
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

/** A constant expression is optimized to a constant. */
class OptimizeConstantExpression : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** A non constant expression is not optimized. */
class NonConstant : public NoOptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantTrue : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(1 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** An expression beginning with a single constant is optimized. */
class ConstantNonConstantFalse : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(0 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
};

/** An expression with a field path and '1'. */
class NonConstantOne : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY("$a" << 1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** An expression with a field path and '0'. */
class NonConstantZero : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY("$a" << 0));
    }
    BSONObj expectedOptimized() override {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
};

/** An expression with two field paths and '1'. */
class NonConstantNonConstantOne : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY("$a" << "$b" << 1));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** An expression with two field paths and '0'. */
class NonConstantNonConstantZero : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY("$a" << "$b" << 0));
    }
    BSONObj expectedOptimized() override {
        return BSON("$or" << BSON_ARRAY("$a" << "$b"));
    }
};

/** An expression with '0', '1', and a field path. */
class ZeroOneNonConstant : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(0 << 1 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

/** An expression with '0', '0', and a field path. */
class ZeroZeroNonConstant : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(0 << 0 << "$a"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$or" << BSON_ARRAY("$a"));
    }
};

/** Nested $or expressions. */
class Nested : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(0 << BSON("$or" << BSON_ARRAY(0)) << "$a"
                                          << "$b"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$or" << BSON_ARRAY("$a" << "$b"));
    }
};

/** Nested $or expressions containing a nested value evaluating to false. */
class NestedOne : public OptimizeBase {
    BSONObj spec() override {
        return BSON("$or" << BSON_ARRAY(0 << BSON("$or" << BSON_ARRAY(BSON("$or" << BSON_ARRAY(1))))
                                          << "$a"
                                          << "$b"));
    }
    BSONObj expectedOptimized() override {
        return BSON("$const" << true);
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("expression") {}

    void setupTests() override {
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
    }
};

unittest::OldStyleSuiteInitializer<All> myAll;

}  // namespace Or
}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo

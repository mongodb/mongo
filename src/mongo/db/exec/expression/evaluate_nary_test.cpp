/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

namespace mongo {
namespace expression_evaluation_test {

class ExpressionBaseTest : public unittest::Test {
public:
    void addOperand(boost::intrusive_ptr<ExpressionNary> expr, Value arg) {
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

    boost::intrusive_ptr<ExpressionNary> _expr;
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

    boost::intrusive_ptr<ExpressionNary> _expr;
};

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
    assertEval(std::numeric_limits<int>::min(), std::numeric_limits<int>::min());
    assertEval(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
}

TEST_F(ExpressionTruncTwoArgTest, IntArg2) {
    assertEval(0, 0, 0);
    assertEval(2, -1, 0);
    assertEval(29, -1, 20);
    assertEval(std::numeric_limits<int>::min(), 10, std::numeric_limits<int>::min());
    assertEval(std::numeric_limits<int>::max(), 42, std::numeric_limits<int>::max());
}

TEST_F(ExpressionTruncOneArgTest, LongArg1) {
    assertEval(0LL, 0LL);
    assertEval(std::numeric_limits<long long>::min(), std::numeric_limits<long long>::min());
    assertEval(std::numeric_limits<long long>::max(), std::numeric_limits<long long>::max());
}

TEST_F(ExpressionTruncTwoArgTest, LongArg2) {
    assertEval(0LL, 0LL, 0LL);
    assertEval(2LL, -1LL, 0LL);
    assertEval(29LL, -1LL, 20LL);
    assertEval(std::numeric_limits<long long>::min(), 10LL, std::numeric_limits<long long>::min());
    assertEval(std::numeric_limits<long long>::max(), 42LL, std::numeric_limits<long long>::max());
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
    double largerThanLong = static_cast<double>(std::numeric_limits<long long>::max()) * 2.0;
    assertEval(largerThanLong, largerThanLong);
    double smallerThanLong = std::numeric_limits<long long>::min() * 2.0;
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
    void assertEvaluates(Value input, Value output) override {
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
    void assertEvaluates(Value input, Value output) override {
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
    void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionCeil(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionCeilTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(std::numeric_limits<int>::min()), Value(std::numeric_limits<int>::min()));
    assertEvaluates(Value(std::numeric_limits<int>::max()), Value(std::numeric_limits<int>::max()));
}

TEST_F(ExpressionCeilTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(std::numeric_limits<long long>::min()),
                    Value(std::numeric_limits<long long>::min()));
    assertEvaluates(Value(std::numeric_limits<long long>::max()),
                    Value(std::numeric_limits<long long>::max()));
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
    double largerThanLong = static_cast<double>(std::numeric_limits<long long>::max()) * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = std::numeric_limits<long long>::min() * 2.0;
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
    void assertEvaluates(Value input, Value output) override {
        _expr = new ExpressionFloor(&expCtx);
        ExpressionNaryTestOneArg::assertEvaluates(input, output);
    }
};

TEST_F(ExpressionFloorTest, IntArg) {
    assertEvaluates(Value(0), Value(0));
    assertEvaluates(Value(std::numeric_limits<int>::min()), Value(std::numeric_limits<int>::min()));
    assertEvaluates(Value(std::numeric_limits<int>::max()), Value(std::numeric_limits<int>::max()));
}

TEST_F(ExpressionFloorTest, LongArg) {
    assertEvaluates(Value(0LL), Value(0LL));
    assertEvaluates(Value(std::numeric_limits<long long>::min()),
                    Value(std::numeric_limits<long long>::min()));
    assertEvaluates(Value(std::numeric_limits<long long>::max()),
                    Value(std::numeric_limits<long long>::max()));
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
    double largerThanLong = static_cast<double>(std::numeric_limits<long long>::max()) * 2.0;
    assertEvaluates(Value(largerThanLong), Value(largerThanLong));
    double smallerThanLong = std::numeric_limits<long long>::min() * 2.0;
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
    assertEval(std::numeric_limits<int>::min(), std::numeric_limits<int>::min());
    assertEval(std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
}

TEST_F(ExpressionRoundTwoArgTest, IntArg2) {
    assertEval(0, 0, 0);
    assertEval(2, -1, 0);
    assertEval(29, -1, 30);
    assertEval(std::numeric_limits<int>::min(), 10, std::numeric_limits<int>::min());
    assertEval(std::numeric_limits<int>::max(), 42, std::numeric_limits<int>::max());
}

TEST_F(ExpressionRoundOneArgTest, LongArg1) {
    assertEval(0LL, 0LL);
    assertEval(std::numeric_limits<long long>::min(), std::numeric_limits<long long>::min());
    assertEval(std::numeric_limits<long long>::max(), std::numeric_limits<long long>::max());
}

TEST_F(ExpressionRoundTwoArgTest, LongArg2) {
    assertEval(0LL, 0LL, 0LL);
    assertEval(2LL, -1LL, 0LL);
    assertEval(29LL, -1LL, 30LL);
    assertEval(std::numeric_limits<long long>::min(), 10LL, std::numeric_limits<long long>::min());
    assertEval(std::numeric_limits<long long>::max(), 42LL, std::numeric_limits<long long>::max());
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
    double largerThanLong = static_cast<double>(std::numeric_limits<long long>::max()) * 2.0;
    assertEval(largerThanLong, largerThanLong);
    double smallerThanLong = std::numeric_limits<long long>::min() * 2.0;
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
    assertEval(std::vector<Value>{Value("A"_sd)}, "A"_sd);
    assertEval(std::vector<Value>{Value("A"_sd), Value("B"_sd)}, "A"_sd);
    assertEval(std::vector<Value>{Value("A"_sd), Value("B"_sd), Value("C"_sd)}, "A"_sd);
}

TEST_F(ExpressionFirstTest, HandlesEmptyArray) {
    assertEval(std::vector<Value>{}, Value());
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
    assertEval(std::vector<Value>{Value("A"_sd)}, "A"_sd);
    assertEval(std::vector<Value>{Value("A"_sd), Value("B"_sd)}, "B"_sd);
    assertEval(std::vector<Value>{Value("A"_sd), Value("B"_sd), Value("C"_sd)}, "C"_sd);
}

TEST_F(ExpressionLastTest, HandlesEmptyArray) {
    assertEval(std::vector<Value>{}, Value());
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

}  // namespace expression_evaluation_test
}  // namespace mongo

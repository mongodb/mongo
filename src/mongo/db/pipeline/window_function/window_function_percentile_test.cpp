/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_percentile.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <limits>

namespace mongo {
namespace {

class WindowFunctionPercentileTest : public AggregationContextFixture {
public:
    WindowFunctionPercentileTest()
        : expCtx(getExpCtx()),
          medianApprox(expCtx.get(), PercentileMethodEnum::kApproximate),
          medianDiscrete(expCtx.get(), PercentileMethodEnum::kDiscrete) {}

    const double kNaN = std::numeric_limits<double>::quiet_NaN();

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionMedian medianApprox;
    WindowFunctionMedian medianDiscrete;
};

TEST_F(WindowFunctionPercentileTest, NaNOnlyWindowReturnsNull) {
    medianApprox.add(Value(kNaN));
    medianApprox.add(Value(kNaN));
    ASSERT_VALUE_EQ(medianApprox.getValue(), Value{BSONNULL});
}

TEST_F(WindowFunctionPercentileTest, NaNMixedWithFiniteFiltered) {
    medianApprox.add(Value(1.0));
    medianApprox.add(Value(kNaN));
    medianApprox.add(Value(2.0));
    ASSERT_VALUE_EQ(medianApprox.getValue(), Value(1.0));
}

TEST_F(WindowFunctionPercentileTest, RemoveFiniteAfterNaNDoesNotCorrupt) {
    medianDiscrete.add(Value(1.0));
    medianDiscrete.add(Value(kNaN));
    medianDiscrete.remove(Value(1.0));
    medianDiscrete.add(Value(3.0));
    ASSERT_VALUE_EQ(medianDiscrete.getValue(), Value(3.0));
}

TEST_F(WindowFunctionPercentileTest, DecimalNaNIsFiltered) {
    medianApprox.add(Value(1.0));
    medianApprox.add(Value(Decimal128::kPositiveNaN));
    medianApprox.add(Value(3.0));
    medianApprox.remove(Value(Decimal128::kPositiveNaN));
    ASSERT_VALUE_EQ(medianApprox.getValue(), Value(1.0));
}

}  // namespace
}  // namespace mongo

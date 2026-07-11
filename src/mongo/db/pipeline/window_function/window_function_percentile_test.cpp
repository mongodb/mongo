// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

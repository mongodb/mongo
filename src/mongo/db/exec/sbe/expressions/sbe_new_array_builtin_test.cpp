// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::sbe {

class SBEBuiltinNewArrayTest : public EExpressionTestFixture {
protected:
    std::unique_ptr<EExpression> makeNewArrayOfLargeStrings(size_t count, size_t size) {
        EExpression::Vector args;
        args.reserve(count);
        const std::string largeStr(size, 'a');
        for (size_t i = 0; i < count; ++i) {
            args.emplace_back(makeStringConstant(largeStr));
        }
        return makeFunction(EFn::kNewArray, std::move(args));
    }
};

TEST_F(SBEBuiltinNewArrayTest, NewArrayWithinLimitSucceeds) {
    auto newArrayExpr = makeNewArrayOfLargeStrings(10, 1024);
    auto compiledExpr = compileExpression(*newArrayExpr);

    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::TagValueOwned guard = value::TagValueOwned::fromRaw(tag, val);

    ASSERT(value::isArray(tag));
    ASSERT_EQUALS(value::getArrayView(val)->size(), 10u);
}

TEST_F(SBEBuiltinNewArrayTest, NewArrayExceedsMemoryLimit) {
    auto newArrayExpr = makeNewArrayOfLargeStrings(10, 1024);
    auto compiledExpr = compileExpression(*newArrayExpr);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         10 * 1024};
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace mongo::sbe

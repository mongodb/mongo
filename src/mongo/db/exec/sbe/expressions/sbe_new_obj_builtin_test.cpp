// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::sbe {

class SBEBuiltinNewObjTest : public EExpressionTestFixture {
protected:
    std::unique_ptr<EExpression> makeNewObjOfLargeValues(size_t numFields, size_t valueSize) {
        EExpression::Vector args;
        args.reserve(numFields * 2);
        const std::string largeStr(valueSize, 'a');
        for (size_t i = 0; i < numFields; ++i) {
            args.emplace_back(makeStringConstant("f" + std::to_string(i)));
            args.emplace_back(makeStringConstant(largeStr));
        }
        return makeFunction(EFn::kNewObj, std::move(args));
    }
};

TEST_F(SBEBuiltinNewObjTest, NewObjWithinLimitSucceeds) {
    auto newObjExpr = makeNewObjOfLargeValues(10, 1024);
    auto compiledExpr = compileExpression(*newObjExpr);

    auto [tag, val] = runCompiledExpression(compiledExpr.get());
    value::ValueGuard guard(tag, val);

    ASSERT(value::isObject(tag));
    ASSERT_EQUALS(value::getObjectView(val)->size(), 10u);
}

TEST_F(SBEBuiltinNewObjTest, NewObjExceedsMemoryLimit) {
    auto newObjExpr = makeNewObjOfLargeValues(10, 1024);
    auto compiledExpr = compileExpression(*newObjExpr);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                         10 * 1024};
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

TEST_F(SBEBuiltinNewObjTest, NewObjLongFieldNamesCountTowardMemoryLimit) {
    EExpression::Vector args;
    const std::string longKey(200, 'k');
    for (int i = 0; i < 10; ++i) {
        args.emplace_back(makeStringConstant(longKey + std::to_string(i)));
        args.emplace_back(makeInt32Constant(1));
    }
    auto newObjExpr = makeFunction(EFn::kNewObj, std::move(args));
    auto compiledExpr = compileExpression(*newObjExpr);

    unittest::ServerParameterGuard limit{"internalQueryMaxSingleExpressionMemoryUsageBytes", 500};
    ASSERT_THROWS_CODE(runCompiledExpression(compiledExpr.get()),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace mongo::sbe

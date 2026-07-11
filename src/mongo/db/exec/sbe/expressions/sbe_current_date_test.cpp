// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>

namespace mongo::sbe {

using SBECurrentDateTest = EExpressionTestFixture;

TEST_F(SBECurrentDateTest, BaseCurrentDate) {
    auto expr = sbe::makeE<sbe::EFunction>(EFn::kCurrentDate, sbe::makeEs());
    auto compiledExpr = compileExpression(*expr);

    auto run = [&]() {
        auto start = Date_t::now();

        auto [tag, val] = runCompiledExpression(compiledExpr.get());
        ASSERT_EQUALS(tag, sbe::value::TypeTags::Date);
        auto result = Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(val));
        ASSERT_GREATER_THAN_OR_EQUALS(result, start);
    };

    run();
    sleepFor(Milliseconds{2});
    run();
}

}  // namespace mongo::sbe
